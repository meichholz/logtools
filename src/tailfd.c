/* ======================================================================

tailfd

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software Foundation,
Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

======================================================================

HISTORY - see HISTORY.tailfd

NOTE: There are some "leftovers" from side paths of the evolution
process of this code. Usually they are ifdef'd out.

The program follows a general "work-like-expected-or-die"
policy. Thus, error codes are at most locations not generated, when a
Panic() makes any sense instead.

====================================================================== */

#ifdef LARGE_FILE

#define _FILE_OFFSET_BITS 64
#define ATOL64 atoll
#define PRINTF_LD64 "%lld"

#else

#define ATOL64 atol
#define PRINTF_LD64 "%ld"

#endif

#define TFilepos off_t

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <ctype.h>
#include <time.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>
#include <syslog.h>

/* ====================================================================== */

#define REVISION "Revision: 1.5 $"

#define USAGE \
"usage: %s {options} FILE SLAVE" \
"\n\n(C) Marian Eichholz at freenet.de AG 2001-2005"\
"\n\noptions:"\
"\n\t-V : tell version"\
"\n\t-f : do not daemonize"\
"\n\t-q : quiet mode (only errors are logged)"\
"\n\t-d : set debugging mask <MASK>"\
"\n"\
"\n\t-p <file> : use <file> as PID file"\
"\n\t-s <file> : use <file> as NVRAM"\
"\n\n"

#define DEBUG_CONFIG     0x0001
#define DEBUG_PIPES      0x0002
#define DEBUG_SIGNALS    0x0004
#define DEBUG_BUFFER     0x0008

#define PANIC_USAGE     1
#define PANIC_CONFIG    2
#define PANIC_RUN       3
#define PANIC_CHILD     98
#define PANIC_INTERNAL  99

#define ID_NOPROCESS    -1
#define ID_NOFILE       -1

#define STRING_TERMINATE(a) a[sizeof(a)-1]='\0'

#define DEF_CONFIG_FILE_NAME    "/etc/tailfd.conf"

/* ====================================================================== */

/* some types */

typedef enum { false, true } TBool;

/* options */

static unsigned long      ulDebugMask;
static TBool              bVerbose=true;
static TBool              bDaemonMode=true;

/* from configuration file */
static char *             szStatusFile;
static char *             szMonitoredFile;     /* and name */
static char *             szPidFile;           /* name for PID file */
static char *             szChildCommand;      /* name for the slave/child */

/* flags for Signalling */
static volatile TBool     bAbortRequest = false;
static volatile TBool     bHUPRequest   = false;
static volatile TBool     bPipeDied     = false;

/* some states */
static TBool              bKeepPidFile  = false;
static TBool              bWriteStatus  = false;
static int                hMonitoredFile;  /* the watched file's handle */
static TFilepos           lReadPosition;   /* current reading position */
static FILE              *fhChild;         /* pipe FHandle of the child */
static int                hChild;          /* file descriptor thereof */

#define                   LOG_BUFFER_SIZE 8192
static char               achLogBuffer[LOG_BUFFER_SIZE];

/* **********************************************************************

lprintf(format, ...)

Write a log message entry WITH LF!

********************************************************************** */

static void lprintf(const char *szFormat, ...)
{
  va_list ap;
  char    ach[500];
  int     bBreak=1;
  if (*szFormat=='~') { szFormat++; bBreak=0; }
  va_start(ap,szFormat);
  vsnprintf(ach,sizeof(ach),szFormat,ap);
  STRING_TERMINATE(ach);
  va_end(ap);
  syslog(LOG_DAEMON|LOG_NOTICE, "notice: %s", ach);
}

/* **********************************************************************

dprintf(mask, format, ...)

Write some debugging messages, if the appropriate debugging mask/channel
is activated by the user.

********************************************************************** */

static void dprintf(unsigned long ulType, const char *szFormat, ...)
{
  char    ach[500];
  va_list ap;
  if ((ulDebugMask & ulType)!=ulType) return;
  va_start(ap,szFormat);
  vsnprintf(ach,sizeof(ach),szFormat,ap);
  STRING_TERMINATE(ach);
  va_end(ap);
  syslog(LOG_USER|LOG_DEBUG, "debug: %s", ach);
}

/* **********************************************************************

Panic(error, format, ...)

The program is aborted, all handles and ressources are freed (thus
being global) and the user gets a nice panic screen :-)

This function also is suitable for regular program termination,
for doing any housekeeping (thus avoiding redundancy and code duplication).

********************************************************************** */

static void CloseAllFilesAndPipes(void);
static void WritePidFile(TBool);

static void Panic(int nError, const char *szFormat, ...)
{
  va_list ap;
  char ach[500];
  if (szFormat!=NULL)
    {
      va_start(ap,szFormat);
      vsnprintf(ach,sizeof(ach),szFormat,ap);
      STRING_TERMINATE(ach);
      va_end(ap);
      syslog(LOG_DAEMON|LOG_ERR,"fatal: %s",ach);
    }
  CloseAllFilesAndPipes();
  if (bDaemonMode) WritePidFile(false);
  closelog();
  if (szFormat!=NULL)
    exit(nError);
}

/* **********************************************************************

TellRevision()

Echo Revisionstring (without RCS header) on STDOUT

********************************************************************** */

static void TellRevision(void)
{
  const char *pch=REVISION;
  while (*pch && *pch!=':') pch++; if (*pch) pch++;
  while (*pch && *pch==' ') pch++;
  fprintf(stdout,"%s ",PROG_NAME);
  if (*pch)
    {
      while (*pch && *pch!='$')
	fputc(*pch++,stdout);
    }
  else
    fputs("unknown",stdout);
  fprintf(stdout,"\n");
}

/* **********************************************************************

CatchAbortRequest()

The sigaction() handler.
It does not use the siginfo_t value for portability reasons.
With that value, the child identification could be made somewhat
easier.

SIGPIPE and SIGCHLD are handled differently, because they really
arrive at different times. A dying child issues the SIGCHLD
anychronously. A SIGPIPE by a broken subpipe arrives after the next
write() call.

********************************************************************** */

static void CatchAbortRequest(int idSignal, siginfo_t * psi, void * pDummy2)
{
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, idSignal);
  sigprocmask(SIG_BLOCK, &set, NULL);
  dprintf(DEBUG_SIGNALS,"got a %d signal!\n",idSignal);
  switch (idSignal)
    {
    case SIGCHLD:
      {
	pid_t id,idLast;
	int nStatus;
	id=0; /* dummy */
	do { idLast=id; id=waitpid(-1,&nStatus,WNOHANG); } while (id>=0);
	dprintf(DEBUG_SIGNALS,"destination process %d died!\n",idLast);
      }
      /* fall through */
    case SIGPIPE:
      bPipeDied=true; /* used as abortive information */
      break;
    case SIGINT:
    case SIGTERM:
      bAbortRequest=true;
      break;
    case SIGHUP:
      bHUPRequest=true;
      break;
    default:
      Panic(PANIC_INTERNAL,"illegal signal %d caught",idSignal);
    }
  sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/* **********************************************************************

SetSignalHandler(bSet)

Install or deinstall the signal handler for HUP, INT, TERM.

********************************************************************** */

static void SetSignalHandler(TBool bSetit)
{
  static struct sigaction sigCatcher;

  memset(&sigCatcher,0,sizeof(sigCatcher));

  sigCatcher.sa_sigaction=CatchAbortRequest;
  sigemptyset(&sigCatcher.sa_mask);
  if (bSetit)
    {
      sigaction(SIGCHLD, &sigCatcher, NULL);
      sigaction(SIGHUP,  &sigCatcher, NULL);
      sigaction(SIGINT,  &sigCatcher, NULL);
      sigaction(SIGTERM, &sigCatcher, NULL);
      sigaction(SIGPIPE, &sigCatcher, NULL);
    }
}

/* **********************************************************************

ChopLine(CRLFedline)

chop off in line CR and LF at line end, if they are present.

Return code: Just the start of the buffer itself.

********************************************************************** */

static char* ChopLine(char *pchLF)
{
  int cch=strlen(pchLF);
  if (cch && pchLF[cch-1]=='\n') pchLF[--cch]='\0';
  if (cch && pchLF[cch-1]=='\r') pchLF[--cch]='\0';
  return pchLF;
}

/* **********************************************************************

WriteStatusFile()

Write the Status File.

Return code: Always 0.

********************************************************************** */

static int WriteStatusFile(void)
{
  FILE *fh;
  fh=fopen(szStatusFile,"w");
  if (!fh) Panic(PANIC_RUN,"cannot create status file \"%s\"",szStatusFile);
  fprintf(fh,"position:" PRINTF_LD64 "\n",lReadPosition);
  fflush(fh);
  if (ferror(fh) || fclose(fh))
    {
      /* implicit close, including close on error-on-close */
      bWriteStatus=false;
      Panic(PANIC_RUN,"error writing status file \"%s\" (%m)",
	    szStatusFile);
    }
  return 0;
}

/* **********************************************************************

ReadStatusFile()

Read the Status File (name is global) or initialise working parameters.

Return code:
   -1 : The file does not exist
    0 : otherwise.

The RV is currently not used.

********************************************************************** */

static int ReadStatusFile(void)
{
  FILE *fh;
  char  achLine[128];
  fh=fopen(szStatusFile,"r");
  if (!fh)
    {
      if (bVerbose)
	lprintf("file %s not found, using defaults.",szStatusFile);
      lReadPosition=0;
      return -1;
    }
  while (!feof(fh))
    {
      char *szKey,*szVal;
      *achLine='\0';
      fgets(achLine,sizeof(achLine),fh);
      if (!*achLine) continue;
      STRING_TERMINATE(achLine);
      ChopLine(achLine);
      szKey=strtok(achLine,":");
      szVal=strtok(NULL,":");
      if (!strcmp(szKey,"position"))
	lReadPosition=ATOL64(szVal);
      else
	Panic(PANIC_RUN,"unknown token %s (%s)",szKey,szVal);
	
    } 
  fclose(fh);
  return 0;
}

/* **********************************************************************

ShutdownDestination()

The destination descriptor is shut down.

Return code: Always 0.

********************************************************************** */

static int ShutdownDestination()
{
  if (fhChild) /* standard descriptors are ok */
    {
      dprintf(DEBUG_PIPES,"closing child fd\n");
      pclose(fhChild);
    }
  fhChild=NULL;
  dprintf(DEBUG_PIPES,"shutdown complete\n");
  return 0;
}

/* **********************************************************************

CloseAllFilesAndPipes()

During regular shotdown as well as panic shutdown all open ends must be
closed and shut down.

********************************************************************** */

static void CloseAllFilesAndPipes(void)
{
  if (bWriteStatus)
    {
      bWriteStatus=false; /* inhibit recursion */
      WriteStatusFile();
      bWriteStatus=true;
    }
  ShutdownDestination();
  if (hMonitoredFile>=0) close(hMonitoredFile);
  hMonitoredFile=0;
}

/* **********************************************************************

RestartDestination()

The specified client is to be started or restarted. So we shut it
down, if necessary, and (re)open it.

Return code:
  -1 : The shutdown failed.
   0 : Otherwise.

********************************************************************** */
  
static int RestartDestination(void)
{
  dprintf(DEBUG_PIPES,"(re)starting dest\n");
  /* Close old pipe, or file, or whatever might still be alive */
  if (ShutdownDestination()<0) /* will not happen */
    return -1;

  /* not output file juggling, no nothing. */
  bPipeDied=false;
  fhChild=popen(szChildCommand,"w");
  if (!fhChild)
    Panic(PANIC_RUN,"cannot popen destination pipe (%m)");
  hChild=fileno(fhChild); /* cache the fd */
  return 0;
}


/* **********************************************************************

WriteToDestination(pchBuffer,cch)

Echo buffer content to destination, using it's internal or pipe interface.
Can write in chunks, if the destination demands for that.

There are *some* ways to come into trouble.
One way is SIGCHLD from a destination. It can be catched and flagged
withing the signal handler.

A more subtle one is the death of an inherited pipe. It leads to a
write error.

Return code: 0 on success, -1 otherwise

********************************************************************** */

static int WriteToDestination(const char *pchBuffer, int cch)
{
  int   cchWritten;
  if (!fhChild) return 0;      /* inactive Pipe handle */
  if (bPipeDied) return -1;
  dprintf(DEBUG_PIPES,"outputting content: %d byte(s)\n",cch);
  do {
    errno=0;
    cchWritten = write(hChild,pchBuffer, cch); /* unbuffered */
    // perror("debug:");
    if (errno==EPIPE) bPipeDied=1; /* won't happen, even the shell will catch it */
    dprintf(DEBUG_PIPES,"%d from %d byte(s) written (errno=%d)\n",
	    cchWritten,cch,errno);
    if (cchWritten>0)
      {
	cch-=cchWritten;
	pchBuffer+=cchWritten;
      }
  } while (cchWritten>=0 && cch>0);
  return (cchWritten>0 && !bPipeDied) ? 0 : -1;
}

/* **********************************************************************

cch = ReadFromFile(file_handle, write_ptr, cchMax)

Read <count> byte from file buffer and write it to <write_ptr>.

********************************************************************** */

static int ReadFromFile(int fdFile, char *pchTo, int cchMax)
{
  int cchOut;
  cchOut=read(fdFile,
	      pchTo,
	      cchMax);
  dprintf(DEBUG_BUFFER,"buffer: read %d byte(s) (errno=%d)\n",
	 cchOut,(int)errno);
  return cchOut;
}

/* **********************************************************************

WriteRestartable(pchBuffer,cch)

Write to destination and restart it if neccessary.
Panics on error.

********************************************************************** */

static void WriteRestartable(const char *achLogBuffer, int cchBuffer)
{
  /*
   * do exact 2 attempts to flush the buffer
   */
  if (WriteToDestination(achLogBuffer,cchBuffer))
    {
      int bFailed=1;
      if (!RestartDestination())
	{
	  if (!WriteToDestination(achLogBuffer,cchBuffer))
	    {
	      sleep(1); /* allow a SIGPIPE to arrive this time! */
	      if (!bPipeDied)
		bFailed=0;
	    }
	}
      if (bFailed)
	Panic(PANIC_RUN,"destination failed twice, aborting...");
    }
}

/* **********************************************************************

i=SearchNewLine(achBuffer,cch)

returns first index of a NL in a buffer.
returns -1 if there is no NL

********************************************************************** */

static int SearchNewLine(const char *pchBuffer, int cch)
{
  int i,iLast;
  iLast=-1;
  for (i=0; i<cch; i++)
    if (pchBuffer[i]=='\n')
      iLast=i;
  return iLast;
}



/* **********************************************************************

MonitorFile()

Seek to the last position of the open file and watch it changing :-)

********************************************************************** */

static void MonitorFile(void)
{
  int         cLoops;
  TFilepos    lFileIndex,lPosWritten;
  ino_t       iNode;          /* inode of open file */
  struct stat statFD;
  time_t      tiLastUpdate;
  int         iEOB; /* end of buffer AFTER last character */

  /*
    since lseek allows for seeking beyond EOF, we have do to the bounds
    check manually.
  */
  lPosWritten=lReadPosition;
  if (fstat(hMonitoredFile,&statFD)<0)
    Panic(PANIC_RUN,"cannot fstat monitored fd: %m");
  lFileIndex=statFD.st_size;
  iNode=statFD.st_ino;
  if (lFileIndex<lReadPosition)
    {
      if (bVerbose)
	lprintf("file size<lastpos, restarting at beginning");
      lReadPosition=lseek(hMonitoredFile, 0, SEEK_SET);
    }
  else if (lseek(hMonitoredFile, lReadPosition, SEEK_SET)!=lReadPosition)
    Panic(PANIC_RUN,"cannot seek to " PRINTF_LD64 ,lReadPosition);
  bWriteStatus=true;
  tiLastUpdate=time(NULL);
  cLoops=0;
  iEOB=0;

  while (!bAbortRequest && !bHUPRequest)
    {
      int cchRead;
      /* update Status file every 3 seconds, if anything happened */
      if (lPosWritten!=lReadPosition && tiLastUpdate+3 < time(NULL))
	{
	  WriteStatusFile();
	  tiLastUpdate=time(NULL);
	}
      /*
       * fill the buffer to the maximum
       */
      cchRead=ReadFromFile(hMonitoredFile,achLogBuffer+iEOB,LOG_BUFFER_SIZE-iEOB); /* non blocking */
      /* dprintf(DEBUG_BUFFER,"i=%d, ch=<%c>\n",i,achLine[i]); */
      cLoops++;
      if (cchRead>0) /* if there is something new */
	{
	  iEOB+=cchRead;
	  int iNL;
	  while (
		 !bAbortRequest
		 &&
		 ((iNL=SearchNewLine(achLogBuffer,iEOB))>=0 || iEOB==LOG_BUFFER_SIZE)
		 )
	    {
	      int i;
	      if (iNL<0) iNL=iEOB-1; /* if no NL and full buffer, flush whole buffer */
	      WriteRestartable(achLogBuffer,iNL+1);
	      /* delete the line(s) */
	      for (i=0; i<iEOB-iNL-1; i++)
		achLogBuffer[i]=achLogBuffer[i+iNL+1]; /* or just memmove()? */
	      iEOB-=iNL+1;
	    }
	}
      else /* nothing in read buffer */
	{
	  int   cRetries;
	  /* wait ONE second, but check abort/HUP */
	  for (cRetries=0;
	       cRetries>=0 && !bAbortRequest && !bHUPRequest;
	       cRetries--)
	      sleep(1);
	  if (bHUPRequest || bAbortRequest) break; /* break whole master loop */
	  /* BEGIN: hup-rollover-block */
	  {
	    TBool bReopen=false;
	    cRetries=20;
	    while (stat(szMonitoredFile,&statFD)<0)
	      {
		sleep(1);
		if (!cRetries--)
		  Panic(PANIC_RUN,"cannot restat \"%s\": %m",
			szMonitoredFile);
	      }
	    if (statFD.st_ino!=iNode)
	      {
		if (bVerbose)
		  lprintf("inode of %s changed, restarting",szMonitoredFile);
		bReopen=true;
		iNode=statFD.st_ino;
	      }
	    else if (lReadPosition>statFD.st_size)
	      {
		if (bVerbose)
		  lprintf("truncation of %s, restarting",szMonitoredFile);
		bReopen=true;
	      }
	    if (bReopen)
	      {
		close(hMonitoredFile);
		hMonitoredFile=open(szMonitoredFile,O_RDONLY);
		if (hMonitoredFile<0)
		  Panic(PANIC_RUN,"cannot open continuation log \"%s\"",
			szMonitoredFile);
		lReadPosition=0; /* update line status */
	      }
	  }  /* END: hup-rollover-block */
	} /* if "nothing in buffer" */
    } /* if not aborted or HUPed */
  /* test abort request */
  if (bVerbose)
    lprintf("MonitorFile() terminated. AbortRequest=%d, HUPRequest=%d, loops=%d",
	    bAbortRequest,bHUPRequest,cLoops);
  if (!bAbortRequest && !bHUPRequest)
    Panic(PANIC_INTERNAL,"internal error: line buffer overflowed");

  WriteStatusFile();
  bWriteStatus=false;
}

/* **********************************************************************

SetDefaultFileName()

********************************************************************** */

char *SetDefaultFileName(const char *szMasterName,
			 const char *szSuffix)
{
  int   cchMaster,cchTotal;
  char *szNewName;
  cchMaster=strlen(szMasterName);
  cchTotal=cchMaster+strlen(szSuffix)+1;
  szNewName=malloc(cchTotal);
  if (!szNewName) Panic(PANIC_CONFIG,"no memory");
  strcpy(szNewName,szMasterName);
  strcpy(szNewName+cchMaster,szSuffix);
  return szNewName;
}

/* **********************************************************************

CheckPidFile()

Check, if we can safely remove the PID file

********************************************************************** */

static void CheckPidFile(void)
{
  FILE *fhPID;
  if (!bDaemonMode) return;
  bKeepPidFile=true; /* nobody clears the existing PID file, ok? */
  fhPID=fopen(szPidFile,"r");
  if (fhPID) /* stale PID? */
    {
      int id=-1;
      if (fscanf(fhPID,"%d",&id)!=1)
	Panic(PANIC_RUN,"cannot read stale PID");
      if (id>0)
	{
	  if (kill(id,0)==0) /* just test process existence */
	    Panic(PANIC_RUN,"another instance is running");
	}
      fclose(fhPID);
    }
  bKeepPidFile=false; /* kick it! */
}

/* **********************************************************************

WritePidFile(TBool bCreate)

Writes or deleted the PID file, if we are in Daemon Mode

********************************************************************** */

static void WritePidFile(TBool bCreate)
{
  FILE *fhPID;
  TBool bOk;
  if (!bDaemonMode) return;
  if (bCreate)
    {
      fhPID=fopen(szPidFile,"w");
      bOk=(fhPID!=NULL);
      if (fhPID)
	{
	  fprintf(fhPID,"%d\n",(int)getpid());
	  if (ferror(fhPID)) bOk=false;
	  fflush(fhPID);
	  if (ferror(fhPID)) bOk=false;
	  fclose(fhPID);
	}
      if (!bOk)
	 Panic(PANIC_CONFIG,"cannot create PID file [%m]");
    }
  else if (!bKeepPidFile)
    {
      unlink(szPidFile);
    }
}

/* **********************************************************************

OpenMonitoredFile()

This little helper is just for the juggling with the file handles to get
an fd > 2.

********************************************************************** */

void OpenMonitoredFile(void)
{
  int hTemp=open(szMonitoredFile,O_RDONLY);
  hMonitoredFile=-1;
  if (hTemp<0)
    Panic(PANIC_RUN,"cannot open \"%s\" [%m]",szMonitoredFile);
  hMonitoredFile=fcntl(hTemp,F_DUPFD,3);
  close(hTemp);
  if (hMonitoredFile<0)
    Panic(PANIC_RUN,"cannot fdup \"%s\" [%m]",szMonitoredFile);
}

/* **********************************************************************

Daemonize()

Do everything, that makes a nice daemon from a normal process

********************************************************************** */

void Daemonize(void)
{
  int rc;

  close(2); close(1); close (0);
  /* detach from terminal, but set the standard descriptors */

  setsid();                   /* become own leader */
  setpgrp();                  /* start own process group */
  umask(0077);

  rc=fork();      /* detach from shell: fork and exit */
  if (rc<0) Panic(DEBUG_CONFIG,"cannot daemonize [%m]");
  else if (rc>0) _exit(0); /* parent path */
}

/* ============================== MAIN ============================== */

int main(int cArg, char * const ppchArg[])
{
  char   achConfigName[256];
  char   chOpt;
  
/*
DDD param:        -d 1 -f testlog cat
                  -c tailfd.conf 
*/

  strcpy(achConfigName,DEF_CONFIG_FILE_NAME);
  
  while (EOF!=(chOpt=getopt(cArg,ppchArg,"Vqfhp:s:d:")))
    {
      switch (chOpt)
	{
	  /* standard options */
	case 'h':
	  printf(USAGE,PROG_NAME);
	  exit(0);
	  break;
	case 'd': ulDebugMask = strtoul(optarg,NULL,10); break;
	case 'q': bVerbose    = false; break;
	case 'V': TellRevision(); exit(0); break;
	    /* specific options */
	case 'f': bDaemonMode = false; break;
	case 'p': szPidFile = strdup(optarg); break;
	case 's': szStatusFile = strdup(optarg); break;
	}
    }
  
  /* prepare logger */
  openlog(PROG_NAME,
	  bDaemonMode ? LOG_PID : LOG_PERROR,
	  LOG_DAEMON);

  if (optind!=cArg-2)
    {
      printf(USAGE,PROG_NAME);
      exit(PANIC_USAGE);
    }

  /* open loggable file */
  szMonitoredFile=ppchArg[optind];
  szChildCommand=ppchArg[optind+1];
  if (!szPidFile)    szPidFile   =SetDefaultFileName(szMonitoredFile,".pid");
  if (!szStatusFile) szStatusFile=SetDefaultFileName(szMonitoredFile,".status");

  OpenMonitoredFile();

  if (bVerbose)
    lprintf("daemon started");

  /* recap lReadPosition */
  ReadStatusFile();

  SetSignalHandler(true);

  CheckPidFile();

  if (bDaemonMode) Daemonize(); /* again and again */

  while (1)
    {
      WritePidFile(true);
      RestartDestination();
      MonitorFile();
      WritePidFile(false);
      /*
	loop ends on either
	HUP-Request (redo from start) or
	error (break and exit)
      */
      if (!bHUPRequest)
	break;
      if (bVerbose)
	lprintf("got a HUP request, restarting child process...");
      bHUPRequest=false; /* clear signal */
      ShutdownDestination();   /* close pipes and childs */
      close(hMonitoredFile); hMonitoredFile = ID_NOFILE;

      ReadStatusFile();
      OpenMonitoredFile();
    }

  SetSignalHandler(false);
  CloseAllFilesAndPipes();

  if (bVerbose)
    lprintf("daemon terminated");

  Panic(0,NULL); /* terminate gracefully */

  return 0;
}

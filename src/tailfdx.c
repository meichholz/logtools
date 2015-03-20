/* ======================================================================

tailfd

$Id: tailfdx.c,v 1.2 2002/01/08 18:42:08 eichholz Exp $

HISTORY

05.02.2001 : Man page and all that stuff. Some cleanup.

28.02.2001 : Respawning and dead pipe management stabilized.

22.02.2001 : SIGTERM/INT and truncation rollover work.
             External commands kinda untested.

19.02.2001 : Start.

NOTE: There are some "leftovers" from side paths of the evolution
process of this code. Usually they are ifdef'd out.

The program follows a general "work-like-expected-or-die"
policy. Thus, error codes are at most locations not generated, when a
Panic() makes any sense instead.

   ====================================================================== */

#include "config.h"

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

#define USAGE \
"usage: %s {options} FILE" \
"\n\n(C) Marian Eichholz at freenet.de AG 2001"\
"\n\noptions:"\
"\n\t-V : tell version"\
"\n\t-f : do not daemonize"\
"\n\t-q : quiet mode (only errors are logged)"\
"\n\t-d : set debugging mask <MASK>"\
"\n\t-c : use config file <CONFIGFILE>"\
"\n\t-t : allow monitored file to be <SECONDS> unavailable"\
"\n\t-r : restart broken destinations (or die)"\
"\n\n"

#define DEBUG_CONFIG     0x0001
#define DEBUG_PIPES      0x0002
#define DEBUG_SIGNALS    0x0004

#define PANIC_USAGE     1
#define PANIC_CONFIG    2
#define PANIC_RUN       3
#define PANIC_CHILD     98
#define PANIC_INTERNAL  99

#define ID_NOPROCESS    -1
#define ID_NOFILE       -1

#define x_CHILD_PANIC

#define STRING_TERMINATE(a) a[sizeof(a)-1]='\0'

#define DEF_CONFIG_FILE_NAME    "/etc/tailfd.conf"
#define DEF_STATUS_FILE_NAME    "/var/run/tailfd.status"

#ifndef RUN_DIR
#define RUN_DIR                 "/var/run"
#endif

/* ====================================================================== */

/* some types */

struct TDestination {
  char           *szAlias;          /* logical name, guaranteed to exist */
                                    /* everything else can be NULL or -1 */
  struct TDestination *pNext;       /* next destination in chain */
  volatile enum { dead,    /* ignore this destination */
		  running, /* AFAK destination accepts input */
		  broken   /* destination has a (temporary?) problem */
                  } status;
  /*
    NOTE:
    For ressource management, only the handles are relevant.
    The "status" member is an indicator for the "front side", how to
    deal with the specific destination.
  */
  int             hPipe;            /* pipe handle */
  pid_t           idProcess;        /* pid */
  char           *szCommandline;    /* path to binary */
  char          **aszArgs;          /* pointers to arguments */
  char           *szOutputFile;     /* connected to STDOUT */
};

typedef enum { false, true } TBool;

/* options */

static unsigned long      ulDebugMask;
static TBool              bVerbose=true;
static TBool              bDaemonMode=true;
static int                cSecondsForTakeover=5;
static TBool              bRestartBrokenDestinations;

/* from configuration file */
static char *             szStatusFile;
static char *             szMonitoredFile;     /* and name */
static char *             szPidFile;           /* name for PID file */
static char *             szWorkDir;           /* standard directory */

/* flags for Signalling */
static volatile TBool     bAbortRequest = false;
static volatile TBool     bHUPRequest   = false;
static volatile TBool     bPipeDied     = false;

/* some states */
static TBool              bWriteStatus  = false;
static int                iFirstDest;      /* destination to be repeated */
static int                hMonitoredFile;  /* the watched file's handle */
static long               lReadPosition;   /* current reading position */

static struct TDestination *pdestFirst;

/* **********************************************************************

lprintf(format, ...)

Write a log message entry WITH LF!

********************************************************************** */

void lprintf(const char *szFormat, ...)
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

void dprintf(unsigned long ulType, const char *szFormat, ...)
{
  char    ach[500];
  va_list ap;
  if ((ulDebugMask & ulType)!=ulType) return;
  va_start(ap,szFormat);
  vsnprintf(ach,sizeof(ach),szFormat,ap);
  STRING_TERMINATE(ach);
  va_end(ap);
  syslog(LOG_DAEMON|LOG_DEBUG, "debug: %s", ach);
}

/* **********************************************************************

Panic(error, format, ...)

The program is aborted, all handles and ressources are freed (thus
being global) and the user gets a nice panic screen :-)

This function also is suitable for regular program termination,
for doing any housekeeping (thus avoiding redundancy and code duplication).

********************************************************************** */

void CloseAllFilesAndPipes(void);
void WritePidFile(TBool);

void Panic(int nError, const char *szFormat, ...)
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
  fputs(PROG_NAME,stdout);
  fputs(" ",stdout);
  fputs(VERSION,stdout);
  fputs("\n",stdout);
}

/* **********************************************************************

CatchAbortRequest()

The sigaction() handler.
It does not use the siginfo_t value for portability reasons.
With that value, the child identification could be made somewhat
easier.

With the CHILD_PANIC flag is would be possible to exit the daemon
directly and synchronous, when a destination pipe cannot be
established though a failing execvp(), due to a broken "command="
entry or so.

So a failing execve() is handled like any breaking destination.

SIGPIPE and SIGCHLD are handled differently, because they really
arrive at different times. A dying child issues the SIGCHLD
anychronously. A SIGPIPE by a broken subpipe arrives after the next
write() call.

********************************************************************** */

void CatchAbortRequest(int idSignal, siginfo_t * psi, void * pDummy2)
{
  struct TDestination *pdest;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, idSignal);
  sigprocmask(SIG_BLOCK, &set, NULL);
  dprintf(DEBUG_SIGNALS,"got a %d signal!\n",idSignal);
  switch (idSignal)
    {
    case SIGCHLD:
      for (pdest=pdestFirst; pdest; pdest=pdest->pNext)
	if (pdest->idProcess!=ID_NOPROCESS)
	  {
	    int   nStatus;
	    pid_t id=waitpid(pdest->idProcess,&nStatus,WNOHANG);
	    if (id>0)
	      {
		pdest->status=broken;
		dprintf(DEBUG_SIGNALS,"destination process %d [%s] died!\n",
			id,pdest->szAlias);
#ifdef CHILD_PANIC
		if (WEXITSTATUS(nStatus)==PANIC_CHILD)
		    Panic(PANIC_CONFIG,"cannot execute [%s]",
			  pdest->szAlias);
#endif
	      }
	  }
      break;
    case SIGPIPE:
      bPipeDied=true;
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

void SetSignalHandler(TBool bSetit)
{
  static struct sigaction sigCatcher;

  memset(&sigCatcher,0,sizeof(sigCatcher));

  sigCatcher.sa_sigaction=CatchAbortRequest;
  sigemptyset(&sigCatcher.sa_mask);
#ifdef BLOCK_SIGNALS_MUTUALLY
  /* since our handlers are *very* fast, this masks are not *really* needed */
  sigaddset(&sigCatcher.sa_mask,SIGHUP);
  sigaddset(&sigCatcher.sa_mask,SIGCHLD);
  sigaddset(&sigCatcher.sa_mask,SIGPIPE);
  sigaddset(&sigCatcher.sa_mask,SIGTERM);
  sigaddset(&sigCatcher.sa_mask,SIGINT);
#endif
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

char* ChopLine(char *pchLF)
{
  int cch=strlen(pchLF);
  if (cch && pchLF[cch-1]=='\n') pchLF[--cch]='\0';
  if (cch && pchLF[cch-1]=='\r') pchLF[--cch]='\0';
  return pchLF;
}

/* **********************************************************************

SetString(&ppch,szOrig)

Duplicates a string, but releases memory previously allocated. This
way a string can be "overwritten" safely.

Return code: The new string buffer start.

********************************************************************** */

char *SetString(char **ppchDestination, const char *szOriginal)
{
  if (*ppchDestination) free(*ppchDestination);
  *ppchDestination=szOriginal ? strdup(szOriginal) : NULL;
  return *ppchDestination;
}

/* **********************************************************************

FreeArgTokens(ppch)

Free the space allocated by TokenizeArgs(). This is the reason, why
the routines are grouped together.

********************************************************************** */

void FreeArgTokens(char **ppch)
{
  if (ppch)
    {
      free(ppch[1]); /* arg space copy */
      free(ppch);    /* free arg array itself */
    }
}

/* **********************************************************************

ppch=TokenizeArgs(argline)

Decompose an argument string to substrings. First and last entries
will be NULL.

Arguments are separated by one or more WHITESPACE.
Any character may be escaped with a backslash (\" \' \\ \ ).
Additionally the \a-\z codes are supported as well as up to octal
digits, e.g. \001 or just \1.

FREEable objects are ppch and ppch[1] (see above);

Return code: The array of pointers to the arguments. Note, that the
first entry is empty (=NULL), and that the last valid pointer in the
array is NULL, too.

********************************************************************** */

char **TokenizeArgs(const char *szLine)
{
  char       *pchBuffer,*pchWrite,*pchArg;
  const char *pchRead;
  int         cArg,iArg,iPass,cchBuffer;
  char      **ppchArgs=NULL;

  dprintf(DEBUG_CONFIG,"arg[*]=\"%s\"\n",szLine);
  if (!*szLine) return NULL;
  pchWrite=NULL; pchArg=NULL; cArg=1; iArg=1;
  for (iPass=0; iPass<2; iPass++)
    {
      if (iPass==1)
	{
	  pchBuffer=calloc(1,strlen(szLine)); /* get some buffer */
	  ppchArgs=calloc(cArg+2,sizeof(char*));
	  pchWrite=pchBuffer;
	  pchArg=pchBuffer;
	}
      else
	{
	  cchBuffer=1; /* one for the trailing NUL */
	}
      pchRead=szLine;
      while (*pchRead)
	{
	  if (isspace(*pchRead))
	    {
	      if (iPass==1)
		{
		  ppchArgs[iArg++]=pchArg;
		  *pchWrite++ = '\0';
		  pchArg=pchWrite;
		}
	      else
		{
		  cArg++;
		  cchBuffer++;
		}
	      while (isspace(*++pchRead));
	      pchRead--; /* put pack the continuation byte */
	    }
	  else
	    {
	      char ch=*pchRead;
	      if (ch=='\\')
		{
		  char chNext=*++pchRead;
		  if (chNext=='\\')
		    ch=chNext;
		  else if (islower(chNext))
		    ch=chNext-'a';
		  else if (isdigit(chNext))
		    {
		      int i,n;
		      i=3; n=0;
		      while (isdigit(*pchRead) && i--)
			n=(n<<3)|((*pchRead++)-'0');
		      ch=(char)n;
		      pchRead--; /* put the last character back */
		    }
		  else
		    ch=chNext; /* that is quotes or space or so */
		}
	      if (iPass==1)
		*pchWrite++ = ch;
	      else
		cchBuffer++;
	    }
	  pchRead++;
	}
    }
  ppchArgs[iArg]=pchArg;    /* register pending argument */
  ppchArgs[cArg+1]=NULL;    /* fix dummy trailing assignment */
  for (iArg=1; iArg<=cArg; iArg++)
    dprintf(DEBUG_CONFIG,"arg %d=<%s>\n",iArg,ppchArgs[iArg]);
  *pchWrite++='\0';
  return ppchArgs;
}

/* **********************************************************************

FreeDestination(pdest)

Free all memory attached to a destination

********************************************************************** */

void FreeDestination(struct TDestination *pdest)
{
  if (pdest->szAlias) free(pdest->szAlias);
  if (pdest->szCommandline) free(pdest->szCommandline);
  if (pdest->szOutputFile) free(pdest->szOutputFile);
  FreeArgTokens(pdest->aszArgs);   /* free memory 1 */
  free(pdest);                      /* free memory 2 */
}


/* **********************************************************************

ShutdownDestination(pdest)

A destination descriptor is shut down. This function must be repeatable
and thus paranoic.

Return code: Always 0.

********************************************************************** */

int ShutdownDestination(struct TDestination *pdest)
{
  if (pdest->hPipe>=0) /* standard descriptors are ok */
    {
      dprintf(DEBUG_PIPES,"closing fd %d\n",pdest->hPipe);
      close(pdest->hPipe);
      if (pdest->idProcess>0 && pdest->status!=broken)
	{
	  pid_t idProcess=pdest->idProcess;
	  pdest->idProcess=ID_NOPROCESS; /* disable "broken" flagging by racing signal */
	  kill(idProcess,SIGTERM);
	  waitpid(idProcess,NULL,0); /* blocking wait */
	}
    }
  pdest->status    = dead;
  pdest->idProcess = ID_NOPROCESS;
  pdest->hPipe     = ID_NOFILE;
  return 0;
}

/* **********************************************************************

WriteStatusFile()

Write the Status File.

Return code: Always 0.

********************************************************************** */

int WriteStatusFile(void)
{
  FILE *fh;
  char *szFile=szStatusFile;
  if (!szFile) szFile=DEF_STATUS_FILE_NAME;
  fh=fopen(szFile,"w");
  if (!fh) Panic(PANIC_RUN,"cannot create status file \"%s\"",szFile);
  fprintf(fh,"firstpipe:%d\n",iFirstDest);
  fprintf(fh,"position:%ld\n",lReadPosition);
  fflush(fh);
  if (ferror(fh) || fclose(fh))
    {
      /* implicit close, including close on error-on-close */
      bWriteStatus=false;
      Panic(PANIC_RUN,"error writing status file \"%s\" [%m]",
	    szFile);
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

int ReadStatusFile(void)
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
	lReadPosition=atol(szVal);
      else if (!strcmp(szKey,"firstpipe"))
	iFirstDest=atoi(szVal);
      else
	Panic(PANIC_RUN,"unknown token %s (%s)",szKey,szVal);
	
    } 
  fclose(fh);
  return 0;
}

/* **********************************************************************

CloseAllFilesAndPipes()

During regular shotdown as well as panic shutdown all open ends must be
closed and shut down.

********************************************************************** */

void CloseAllFilesAndPipes(void)
{
  struct TDestination *pdest;
  if (bWriteStatus)
    {
      bWriteStatus=false; /* inhibit recursion */
      WriteStatusFile();
      bWriteStatus=true;
    }
  for (pdest=pdestFirst;
       pdest;
       pdest=pdest->pNext)
      ShutdownDestination(pdest);
  if (hMonitoredFile>=0) close(hMonitoredFile);
  hMonitoredFile=0;
}

/* **********************************************************************

RestartDestination(pdest)

The specified client is broken or unconnected to a pipe. So we shut it
down, if necessary, and (re)open it.

Return code:
  -1 : The shutdown failed.
   0 : Otherwise.

********************************************************************** */
  
int RestartDestination(struct TDestination *pdest)
{
  int hStdOut = ID_NOFILE;
  /* Close old pipe, or file, or whatever might still be alive */
  if (ShutdownDestination(pdest)<0)
    return -1;
  dprintf(DEBUG_PIPES,"restarting [%s]\n",pdest->szAlias);

  if (pdest->szOutputFile)
    {
      int hTemp; /* open()-handle, will be transferred to fd>2 */
      int nAppendFlag=O_TRUNC;
      char *sz=pdest->szOutputFile;
      if (*sz == '>')
	{
	  sz++;
	  nAppendFlag=O_APPEND;
	}
      hTemp = open(sz, O_CREAT|O_WRONLY|nAppendFlag, 00666);
      if (hTemp>=0)
	{
	  hStdOut=fcntl(hTemp,F_DUPFD,3);
	  close(hTemp);
	}
      else
	hStdOut=0;
      dprintf(DEBUG_PIPES,"created fd %d from file %s\n",
	      hStdOut,sz);
      if (hStdOut<3)
	Panic(PANIC_RUN,"cannot create output file for \"%s\"",pdest->szAlias);
    }
  if (pdest->szCommandline)
    {
      int   hIn,hOut;
      {
	int   afdPipe[2];
	if (pipe(afdPipe)<0)
	  Panic(PANIC_RUN,"cannot create pipe fds [%s] %m",pdest->szAlias);
	hIn =fcntl(afdPipe[0],F_DUPFD,3);
	hOut=fcntl(afdPipe[1],F_DUPFD,3);
	close(afdPipe[0]);
	close(afdPipe[1]);
      }
      if (hIn<0 || hOut<0)
	Panic(PANIC_RUN,"cannot dupe pipe fds [%s] %m",pdest->szAlias);
      dprintf(DEBUG_PIPES,"got %d[r] and %d[w]\n",hIn,hOut);

      pdest->idProcess = fork();

      if (pdest->idProcess < 0)                     /* fork failed */
	Panic(PANIC_RUN,"fork failed [%m]");

      else if (!pdest->idProcess)                 /* child trunk */
	{
	  struct TDestination *pIter;
	  if (hMonitoredFile>0) close(hMonitoredFile);
	  if (dup2(hIn,0)<0)
	    {
	      syslog(LOG_DAEMON|LOG_ERR,"dup for 0 [%s] %m",pdest->szAlias);
	      _exit(PANIC_CHILD);
	    }
	  if (pdest->szOutputFile)       /* forces some checks on handle */
	    {
	      if (dup2(hStdOut,1)<0)     /* connect FILE to STDOUT */
		{
		  syslog(LOG_DAEMON|LOG_ERR,"dup for 1 [%s] %m",
			 pdest->szAlias);
		  _exit(PANIC_CHILD);
		}
	    }
	  /* close all siblings FD */
	  for (pIter=pdestFirst; pIter; pIter=pIter->pNext)
	    if (pIter->hPipe>2)
	      close(pIter->hPipe);
	  close(hOut);            /* write direction not needed */
	  close(hIn);             /* duped */
	  close(hStdOut);         /* duped */
	  /*
	    Now there shall be only ONE pipe, on FD 0.
	    Max. TWO FD on 1 and 2 are referring a device or file.
	  */
	  execvp(pdest->szCommandline, pdest->aszArgs);
	  syslog(LOG_DAEMON|LOG_ERR,"error: [%s] cannot exec %s: %m",
		 pdest->szAlias,pdest->szCommandline);
	  _exit(PANIC_CHILD); /* used for SIGCHLD handler */
	}

      else                               /* parent trunk */
	{
	  pdest->status    = running;
	  pdest->hPipe     = hOut; /* writing */
	  close(hIn);              /* read direction not needed */
	  close(hStdOut);          /* output file no longer used */
	} /* forking */
    } /* if pipe */
  else
    { /* direct file connection */
      if (hStdOut>=0)
	{
	  pdest->hPipe=hStdOut;
	  pdest->status = running;
	}
      else
	{
	  if (bVerbose)
	    lprintf("destination [%s] with no process or file",pdest->szAlias);
	}
    }
  return 0;
}


/* **********************************************************************

EchoToDestination(szLine, pdest)

Echo line to destination, using it's internal or pipe interface

There are *some* ways to come into trouble.
One way is SIGCHLD from a destination. It can be catched and flagged
withing the signal handler.

A more subtle one is the death of an inherited pipe. It leads to a
write error.

Return code: Always 0

********************************************************************** */

int EchoToDestination(const char *szLine, struct TDestination *pdest)
{
  int   cch,cchWritten,cRetries,idError;
  char *pchError;
  if (pdest->status==dead) return 0; /* inactive destination */
  if (pdest->hPipe<0) return 0;      /* inactive Pipe handle */
  cch=strlen(szLine);
  cRetries=1;
  idError=0;
  bPipeDied=false; /* raised by SIGPIPE */
  
  if (pdest->status==broken)
    {
      if (bRestartBrokenDestinations)
	{
	  dprintf(DEBUG_PIPES,"BROKEN detected for %d, restarting\n",
		  (int)pdest->hPipe);
	  RestartDestination(pdest);
	}
      else
	{
	  lprintf("disabling died destination [%s]",
		  pdest->szAlias);
	  pdest->status=dead;
	  return 0;
	}
    }
  cchWritten = write(pdest->hPipe, szLine, cch);
  dprintf(DEBUG_PIPES,"%d from %d byte(s) written to %d (errno=%d)\n",
	  cchWritten,cch,(int)pdest->hPipe,(int)errno);
  pchError="N.N.";
  while ((bPipeDied || cchWritten!=cch || pdest->status==broken) && cRetries>0)
    {
      if (bRestartBrokenDestinations)
	{
	  if (pdest->status==broken)
	    {
	      lprintf("warning: destination pipe [%s] died, restarting...",
		      pdest->szAlias);
	      pchError="pipe broken";
	      
	    }
	  else if (bPipeDied) /* inherited pipe died */
	    {
	      lprintf("warning: destination [%s] broken, restarting...",
		      pdest->szAlias);
	      bPipeDied=false;
	      pchError="SIGPIPE";
	    }
	  else
	    {
	      if (!idError) idError=errno;
	      lprintf("warning: destination [%s] failed, restarting...",
		      pdest->szAlias);
	      pchError=strerror(idError);
	    }
	  bPipeDied=false;
	  RestartDestination(pdest);
	  sleep(1); /* give pipe a chance to crash */
	  cchWritten = write(pdest->hPipe, szLine, cch);
	  if (cchWritten==cch && !bPipeDied && pdest->status!=broken)
	    break;
	  cRetries--;
	}
      else
	{
	  pdest->status=dead; /* disable further output to destination */
	  lprintf("disabling broken pipe [%s]...",
		  pdest->szAlias);
	  return 0;
	}
    }
  if (!cRetries)
    Panic(PANIC_RUN,"destination [%s] has a permanent problem: %s",
	  pdest->szAlias,pchError);
  return 0;
}

/* **********************************************************************

MonitorFile()

Seek to the last position of the open file and watch it changing :-)

********************************************************************** */

void MonitorFile(void)
{
  char        achLine[1024];
  int         i;
  long        lFileIndex;
  ino_t       iNode;          /* inode of open file */
  struct stat statFD;

  /*
    since lseek allows for seeking beyond EOF, we have do to the bounds
    check manually.
  */
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
    Panic(PANIC_RUN,"cannot seek to %ld",lReadPosition);
  i=0;
  /* we cannot update lReadPosition bytewise, because we probably want
     to recap the line, if a destination crashes.
     So we update it linewise. */
  lFileIndex=lReadPosition;

  bWriteStatus=true;

  while (i<sizeof(achLine)-1 && !bAbortRequest && !bHUPRequest)
    {
      struct TDestination *pdest;
      int cch=read(hMonitoredFile,achLine+i,1); /* non blocking */
      if (cch<=0)
	{
	  int   cRetries;
	  TBool bReopen=false;
	  for (cRetries=2; cRetries && !bHUPRequest; cRetries--)
	      sleep(1);
	  if (bHUPRequest) break; /* break whole master loop */

	  cRetries=cSecondsForTakeover;
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
	  else if (lFileIndex>statFD.st_size)
	    {
	      if (bVerbose)
		lprintf("truncation of %s, restarting",szMonitoredFile);
	      bReopen=true;
	    }
	  if (!bReopen) continue;
	  /*
	    Flush the last line, if there is one available.
	    In this single output line, the destinations
	    are vulnerable, thus losing a line if they crash.
	  */
	  if (i)
	    {
	      achLine[i++]='\n'; achLine[i]='\0';
	      bWriteStatus=false; /* no log of inconsistent data */
	      for (pdest=pdestFirst;
		   pdest;
		   pdest=pdest->pNext)
		EchoToDestination(achLine,pdest);
	      bWriteStatus=true;
	    }
	  close(hMonitoredFile);
	  hMonitoredFile=open(szMonitoredFile,O_RDONLY);
	  if (hMonitoredFile<0)
	    Panic(PANIC_RUN,"cannot open continuation log \"%s\"",
		  szMonitoredFile);
	  lFileIndex=lReadPosition=0; /* update line status */
	  WriteStatusFile();
	  i=0;
	  continue; /* and restart reading from scratch */
	}
      lFileIndex++;
      if (achLine[i]=='\r') i--; /* skip CR */
      else if (achLine[i]=='\n') 
	{
	  int iDestination;
	  achLine[++i]='\0';
	  for (pdest=pdestFirst, iDestination=0;
	       pdest;
	       iDestination++, pdest=pdest->pNext)
	    if (iDestination>=iFirstDest)
	      EchoToDestination(achLine,pdest);
	  iFirstDest=0;           /* no more "rewinding" necessary */
	  lReadPosition=lFileIndex; /* update line status */
	  WriteStatusFile();
	  i=0;
	}
      else
	{
	  i++;
	  if (i+1>=sizeof(achLine))
	    i--; /* forget the trailing garbage */
	}
    }
  /* test abort request */
  if (!bAbortRequest && !bHUPRequest)
    Panic(PANIC_INTERNAL,"internal error: line buffer overflowed");

  WriteStatusFile();
  bWriteStatus=false;
}

/* **********************************************************************

ReadConfigurationFile(szName)

Read an INI style configuration file.

Missing feature: Numerical format is detected, but neighter
checked nor used.

Return code: Always 0

********************************************************************** */

int ReadConfigurationFile(const char *szName)
{
  /* TODO: do some sanity checks on the file name */
  FILE           *fh;
  int             nLine;
  char            achAlias[64];
  struct TDestination *pdest;
  TBool           bCreateDestination;
  enum { unknown, daemon, childs } idPhase;
  idPhase=unknown;
  fh=fopen(szName,"r");
  if (!fh) Panic(PANIC_CONFIG,"cannot open config-file %s",szName);

  dprintf(DEBUG_CONFIG,"CONFIG: %s open\n",szName);

  nLine=0;
  bCreateDestination=0;
  pdest=NULL;

  SetString(&szStatusFile,NULL);
  SetString(&szPidFile,NULL);
  SetString(&szWorkDir,"/");
  
  while (!feof(fh))
    {
      char achLine[256],*pchKey,*pchValue,bNumerical,ch;
      achLine[0]='\0';
      fgets(achLine, sizeof(achLine), fh);
      STRING_TERMINATE(achLine);
      nLine++;
      ChopLine(achLine);
      if (achLine[0]=='#' || achLine[0]==';' || !*achLine)
	continue;
      /*
	get new section name and state
      */
      if (achLine[0]=='[')
	{
	  char *pch=achLine+1;
	  while (*pch && *pch!=']')
	    {
	      *pch=tolower(*pch); /* [AnYTHING] -> [anything] */
	      pch++;
	    }
	  *pch='\0'; /* cut the ] */
	  pch=achLine+1; /* pch points to "anything" */
	  if (!strcmp(pch,"daemon"))
	    idPhase=daemon;
	  else
	    {
	      bCreateDestination=true;
	      idPhase=childs;
	      strncpy(achAlias,pch,sizeof(achAlias));
	      STRING_TERMINATE(achAlias);
	    }
	  dprintf(DEBUG_CONFIG,"CONFIG: phase %d from %s\n",idPhase,pch);
	  continue;
	}
      if (bCreateDestination)
	{
	  struct TDestination *pdestNew;
	  pdestNew=(struct TDestination *)calloc(1,sizeof(struct
							  TDestination));
	  if (pdest)
	    pdest->pNext=pdestNew;
	  else
	    pdestFirst=pdestNew;
	  pdest=pdestNew;
	  pdest->szAlias=strdup(achAlias);
	  pdest->hPipe = ID_NOFILE;
	  if (pdest->aszArgs)
	    pdest->aszArgs[0]=(pdest->szCommandline)
	      ? pdest->szCommandline
	      : "\0";
	  bCreateDestination=false; /* thank You, one time is enough */
	}

      /* --- extract key from line ----------------------------------- */
      pchKey=achLine;
      while (*pchKey && isspace(*pchKey)) pchKey++; 
      pchValue=pchKey;
      while (*pchValue && !isspace(*pchValue) && *pchValue!='=')
	{
	  *pchValue=tolower(*pchValue);
	  pchValue++;
	}
      ch = *pchValue; /* may be the = */
      if (ch)
	*pchValue++='\0'; /* mark end of key(!) */
      else
	Panic(PANIC_CONFIG,"missing key delimiter in line %d of %s\n",
		  nLine,szName);
      /* turn over to the value */
      if (ch!='=')
	{
	  while (*pchValue && isspace(*pchValue)) pchValue++;
	  if (*pchValue++ != '=')
	    Panic(PANIC_CONFIG,"missing = in line %d of %s\n",
		  nLine,szName);
	}
      while (*pchValue && isspace(*pchValue)) pchValue++;
      bNumerical=isdigit(pchValue);
      if (bNumerical)
	{
	  /* check rest of the number */
	  const char *pch=pchValue;
	  while (*++pch)
	      if (!isdigit(*pch))
		Panic(PANIC_CONFIG,"value not numerical in line %d of %s\n",
		  nLine,szName);
	  /* TODO: Extract number and assign it to whatever */
	}
      else
	{
	  int iTerm;
	  /* this version allows for trailing white space */
	  if (*pchValue!='\"')
	      Panic(PANIC_CONFIG,"missing opening quote in line %d of %s\n",
		    nLine,szName);
	  pchValue++;
	  iTerm=strlen(pchValue)-1;
	  while (iTerm>=0 && isspace(pchValue[iTerm])) iTerm--;
	  if (iTerm>=0 && pchValue[iTerm]=='\"')
	    pchValue[iTerm]='\0';
	  else
	    Panic(PANIC_CONFIG,"error on closing quote in line %d of %s\n",
		  nLine,szName);
	}
      dprintf(DEBUG_CONFIG,"got <%s>=<%s>\n",pchKey,pchValue);


      /* --- sort out what to do with key and value ---------------------- */
      switch(idPhase)
	{
	case daemon:
	  if (!strcmp(pchKey,"statusfile"))
	    SetString(&szStatusFile,pchValue);
	  else if (!strcmp(pchKey,"pidfile"))
	    SetString(&szPidFile,pchValue);
	  else if (!strcmp(pchKey,"workdir"))
	    SetString(&szWorkDir,pchValue);
	  else Panic(PANIC_CONFIG,"unknown key %s in line %d of %s\n",
		     pchKey,nLine,szName);
	  break;
	case childs:
	  if (!strcmp(pchKey,"command"))
	    SetString(&(pdest->szCommandline),pchValue);
	  else if (!strcmp(pchKey,"stdout"))
	    SetString(&(pdest->szOutputFile),pchValue);
	  else if (!strcmp(pchKey,"args"))
	    {
	      FreeArgTokens(pdest->aszArgs);
	      pdest->aszArgs=TokenizeArgs(pchValue);
	    }
	  else Panic(PANIC_CONFIG,"unknown key %s in line %d of %s\n",
		     nLine,szName);
	  break;
	default:
	  Panic(PANIC_CONFIG,"value not allowed outside section in line %d of %s\n",
		     nLine,szName);
	}
    }
  fclose(fh);
  return 0;
}

/* **********************************************************************

WritePidFile(TBool bCreate)

Writes or deleted the PID file, if we are in Daemon Mode

********************************************************************** */

void WritePidFile(TBool bCreate)
{
  FILE *fhPID;
  TBool bOk;
  if (!bDaemonMode) return;
  /* if no PID file is specified, calculate a standard one */
  if (!szPidFile)
    {
      szPidFile=(char*)calloc(1,strlen(RUN_DIR)+strlen(PROG_NAME)+6);
      sprintf(szPidFile,"%s/%s.pid",RUN_DIR,PROG_NAME);
    }
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
  else
    {
      unlink(szPidFile);
    }
}

/* **********************************************************************

OpenMonitoredFile()

This little helper is just for the juggling with teh file handles to get
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
  umask(0);

  rc=fork();      /* detach from shell: fork and exit */
  if (rc<0) Panic(DEBUG_CONFIG,"cannot daemonize [%m]");
  else if (rc>0) _exit(0); /* parent path */
}

/* ============================== MAIN ============================== */

int main(int cArg, char * const ppchArg[])
{
  char achConfigName[256];
  char chOpt;
  
/*
DDD param:        -d 1 -f -c tailfd.conf testlog
*/

  strcpy(achConfigName,DEF_CONFIG_FILE_NAME);
  
  while (EOF!=(chOpt=getopt(cArg,ppchArg,"Vqfhd:c:t:r")))
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
	case 'c': strncpy(achConfigName, optarg, sizeof(achConfigName));
	  STRING_TERMINATE(achConfigName);
	  break;
	case 't': cSecondsForTakeover = atoi(optarg); break;
	case 'r': bRestartBrokenDestinations=true; break;
	}
    }
  
  /* prepare logger */
  openlog(PROG_NAME,
	  bDaemonMode ? LOG_PID : LOG_PERROR,
	  LOG_DAEMON);

  ReadConfigurationFile(achConfigName);

  if (optind!=cArg-1)
    {
      printf(USAGE,PROG_NAME);
      exit(PANIC_USAGE);
    }

  if (chdir(szWorkDir)<0)
    Panic(PANIC_CONFIG,"cannot chdir to %s [%m]",szWorkDir);

  /* open loggable file */
  szMonitoredFile=ppchArg[optind];

  OpenMonitoredFile();

  if (bVerbose)
    lprintf("daemon started");

  /* recap lReadPosition */
  ReadStatusFile();

  SetSignalHandler(true);

  while (1)
    {
      struct TDestination *pdest,*pNext;
      
      if (bDaemonMode) Daemonize(); /* again and again */

      WritePidFile(true);

      /* set up destinations, with detached FDs */
      for (pdest=pdestFirst;
	   pdest;
	   pdest=pdest->pNext)
	RestartDestination(pdest);

      sleep(1); /* give childs a moment to crash :-) */
      MonitorFile();

      if (!bHUPRequest)
	break;
      if (bVerbose)
	lprintf("got a HUP request, rereading configuration...");
      bHUPRequest=false; /* clear signal */

      /* shut down as much as possible */
      for (pdest=pdestFirst;
	   pdest;
	   pdest=pNext)
	{
	  pNext=pdest->pNext;           /* pdest will be trashed */
	  ShutdownDestination(pdest);   /* close pipes and childs */
	  FreeDestination(pdest);       /* free node */
	}
      pdestFirst=NULL;
      close(hMonitoredFile); hMonitoredFile = ID_NOFILE;

      WritePidFile(false); /* PID file name may change */

      /* redefine the whole world */
      ReadConfigurationFile(achConfigName);

      /* restart as much much as possible */
      if (chdir(szWorkDir)<0)
	Panic(PANIC_CONFIG,"cannot chdir to %s [%m]",szWorkDir);
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

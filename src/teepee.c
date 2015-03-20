/* ======================================================================

teepee

======================================================================

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

HISTORY

08.12.2005 : correct line buffer, Revision -> 1.5

06.12.2005 : simple buffer (8K), no stdio

04.05.2005 : Fixed bug in input buffer processing (startup)

04.01.2002 : Buffered Input processing

08.03.2001 : Built from scratch, in order not to bloat tee.

The program follows a general "work-like-expected-or-die"
policy. Thus, error codes are at most locations not generated, when a
Panic() makes any sense instead.

====================================================================== */

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <ctype.h>
#include <time.h>

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

/* ====================================================================== */

#define REVISION "Revision: 1.5 $"

#define USAGE \
"usage: %s {options} FILE SLAVE" \
"\n\n(C) Marian Eichholz at freenet.de AG 2001-2005"\
"\n\noptions:"\
"\n\t-V : tell version"\
"\n\t-d : set debugging mask <MASK>"\
"\n\n"

#define DEBUG_CONFIG     0x0001
#define DEBUG_PIPES      0x0002
#define DEBUG_BUFFER     0x0008

#define PANIC_USAGE     1
#define PANIC_RUN       2

#ifndef STDIN
#define STDIN           0
#endif

#define STRING_TERMINATE(a) a[sizeof(a)-1]='\0'

/* ====================================================================== */

/* some types */

typedef enum { false, true } TBool;

/* options */

static unsigned long      ulDebugMask;

static FILE             **afhDestinations;
static int               *afdDestinations;

#define                   LOG_BUFFER_SIZE 8192
static char               achLogBuffer[LOG_BUFFER_SIZE];

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
  fprintf(stderr, "%s: debug: %s\n", PROG_NAME,ach);
}

/* **********************************************************************

CloseAll(void)

Close all pipes and files and ...

********************************************************************** */

static void CloseAll(void)
{
  FILE **pfh;
  if (afhDestinations)
    {
      for (pfh=afhDestinations; *pfh; pfh++)
	{
	  dprintf(DEBUG_PIPES,"closing child FD");
	  pclose(*pfh);
	  *pfh=NULL;
	}
    }
}

/* **********************************************************************

Panic(error, format, ...)

The program is aborted, all handles and ressources are freed (thus
being global) and the user gets a nice panic screen :-)

This function also is suitable for regular program termination,
for doing any housekeeping (thus avoiding redundancy and code duplication).

********************************************************************** */

static int Panic(int nError, const char *szFormat, ...)
{
  va_list ap;
  char ach[500];
  if (szFormat!=NULL)
    {
      va_start(ap,szFormat);
      vsnprintf(ach,sizeof(ach),szFormat,ap);
      STRING_TERMINATE(ach);
      va_end(ap);
      fprintf(stderr,"%s: fatal: %s\n",PROG_NAME,ach);
    }
  CloseAll();
  if (szFormat!=NULL)
    exit(nError);
  return 0; /* for the compiler */
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

WriteToDestination(fd,pchBuffer,cch)

Echo buffer content to destination, using it's internal or pipe interface

There are *some* ways to come into trouble.
One way is SIGCHLD from a destination. It can be catched and flagged
withing the signal handler.

A more subtle one is the death of an inherited pipe. It leads to a
write error.

Return code: 0 on success, -1 otherwise

********************************************************************* */

static int WriteToDestination(int fd, const char *pchBuffer, int cch)
{
  int   cchWritten;
  if (fd<0) return 0;      /* inactive Pipe handle */
  dprintf(DEBUG_PIPES,"outputting content to fd %d: %d byte(s)",fd,cch);
  do {
    errno=0;
    cchWritten = write(fd, pchBuffer, cch); /* unbuffered */
    dprintf(DEBUG_PIPES,"%d from %d byte(s) written (errno=%d)",
	    cchWritten,cch,errno);
    if (cchWritten>0)
      {
	cch-=cchWritten;
	pchBuffer+=cchWritten;
      }
  } while (cchWritten>=0 && cch>0);
  return (cchWritten>0) ? 0 : -1;
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
  dprintf(DEBUG_BUFFER,"buffer: read %d byte(s) (errno=%d)",
	 cchOut,(int)errno);
  return cchOut;
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

MonitorStream()

Watch STDIN.

return code:
  0 : EOF terminated the loop (=broken pipe)
  1 : Error (broken anything, EOF)

********************************************************************** */

static int MonitorStream(void)
{
  int cchRead,iEOB;
  iEOB=0;
  while (1)
    {
      cchRead=ReadFromFile(STDIN,achLogBuffer+iEOB,LOG_BUFFER_SIZE-iEOB); /* blocking */
      if (cchRead>0)
	{
	  int iNL;
	  int *pfd;
	  iEOB+=cchRead;
	  while (
		 ((iNL=SearchNewLine(achLogBuffer,iEOB))>=0 || iEOB==LOG_BUFFER_SIZE)
		 )
	    {
	      int i;
	      if (iNL<0) iNL=iEOB-1; /* if no NL and full buffer, flush whole buffer */
	      for (pfd=afdDestinations; *pfd>=0; pfd++)
		if (WriteToDestination(*pfd,achLogBuffer,iNL+1)!=0)
		  Panic(PANIC_RUN,"Broken Pipe %d",*pfd);
	      /* delete the line(s) */
	      for (i=0; i<iEOB-iNL-1; i++)
		achLogBuffer[i]=achLogBuffer[i+iNL+1]; /* or just memmove()? */
	      iEOB-=iNL+1;
	    }
	}
      else /* end of pipe :-) */
	{
	  dprintf(DEBUG_PIPES,"errno for STDIN: %d/%s",errno,strerror(errno));
	  return 1;
	}
    } /* while */
  /* not reached */ return 0;
}

/* ============================== MAIN ============================== */

int main(int cArg, char * const ppchArg[])
{
  char chOpt;
  int  cPipes,i;
  
/*
DDD param:
*/

  afhDestinations=NULL;
  while (EOF!=(chOpt=getopt(cArg,ppchArg,"Vhd:")))
    {
      switch (chOpt)
	{
	  /* standard options */
	case 'h':
	  printf(USAGE,PROG_NAME);
	  exit(0);
	  break;
	case 'd': ulDebugMask = strtoul(optarg,NULL,10); break;
	case 'V': TellRevision(); exit(0); break;
	}
    }
  
  cPipes=cArg-optind;

  if (cPipes<1)
    {
      printf(USAGE,PROG_NAME);
      exit(PANIC_USAGE);
    }

  afhDestinations=calloc(cPipes+1,sizeof(FILE*)); /* the last will be NULL */
  afdDestinations=calloc(cPipes+1,sizeof(int));   /* the last will be -1 */
  for (i=0; i<=cPipes; afdDestinations[i++]=-1);

  for (i=0; i<cPipes; i++)
    {
      dprintf(DEBUG_CONFIG,"starting %s\n",ppchArg[optind+i]);
      afhDestinations[i]=popen(ppchArg[optind+i],"w");
      if (afhDestinations[i])
	  afdDestinations[i]=fileno(afhDestinations[i]);
      else
	Panic(PANIC_RUN,"cannot start '%s': %s",
	      ppchArg[optind+i],strerror(errno));
    }
  
  if (!afhDestinations) Panic(PANIC_RUN,"memory error");
  /* get and start all destinations */

  if (MonitorStream())
    Panic(PANIC_RUN,"error during read"); /* not reached??? */
  return Panic(0,NULL);
}

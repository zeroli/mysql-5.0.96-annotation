/* Copyright (c) 2000, 2001, 2004, 2006, 2007 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mysys_priv.h"
#include "mysys_err.h"
#include <errno.h>
#ifdef HAVE_PREAD
#include <unistd.h>
#endif

	/* Read a chunk of bytes from a file  */

uint my_pread(File Filedes, byte *Buffer, uint Count, my_off_t offset,
	      myf MyFlags)
{
  uint readbytes;
  int error;
  DBUG_ENTER("my_pread");
  DBUG_PRINT("my",("Fd: %d  Seek: %lu  Buffer: 0x%lx  Count: %u  MyFlags: %d",
		   Filedes, (ulong) offset, (long) Buffer, Count, MyFlags));

  for (;;)
  {
#ifndef __WIN__
    errno=0;					/* Linux doesn't reset this */
#endif
#ifndef HAVE_PREAD
    pthread_mutex_lock(&my_file_info[Filedes].mutex);
    readbytes= (uint) -1;
    error= (lseek(Filedes, offset, MY_SEEK_SET) == -1L ||
	    (readbytes = (uint) read(Filedes, Buffer, Count)) != Count);
    pthread_mutex_unlock(&my_file_info[Filedes].mutex);
#else
    error=((readbytes = (uint) pread(Filedes, Buffer, Count, offset)) != Count);
#endif
    if (error)
    {
      my_errno=errno;
      DBUG_PRINT("warning",("Read only %d bytes off %u from %d, errno: %d",
			    (int) readbytes, Count,Filedes,my_errno));
#ifdef THREAD
      if ((readbytes == 0 || (int) readbytes == -1) && errno == EINTR)
      {
        DBUG_PRINT("debug", ("my_pread() was interrupted and returned %d",
                             (int) readbytes));
        continue;                              /* Interrupted */
      }
#endif
      if (MyFlags & (MY_WME | MY_FAE | MY_FNABP))
      {
	if ((int) readbytes == -1)
	  my_error(EE_READ, MYF(ME_BELL+ME_WAITTANG),
		   my_filename(Filedes),my_errno);
	else if (MyFlags & (MY_NABP | MY_FNABP))
	  my_error(EE_EOFERR, MYF(ME_BELL+ME_WAITTANG),
		   my_filename(Filedes),my_errno);
      }
      if ((int) readbytes == -1 || (MyFlags & (MY_FNABP | MY_NABP)))
	DBUG_RETURN(MY_FILE_ERROR);		/* Return with error */
    }
    if (MyFlags & (MY_NABP | MY_FNABP))
      DBUG_RETURN(0);				/* Read went ok; Return 0 */
    DBUG_RETURN(readbytes);			/* purecov: inspected */
  }
} /* my_pread */


	/* Write a chunk of bytes to a file */

uint my_pwrite(int Filedes, const byte *Buffer, uint Count, my_off_t offset,
	       myf MyFlags)
{
  uint writenbytes,errors;
  ulong written;
  DBUG_ENTER("my_pwrite");
  DBUG_PRINT("my",("Fd: %d  Seek: %lu  Buffer: 0x%lx  Count: %d  MyFlags: %d",
		   Filedes, (ulong) offset, (long) Buffer, Count, MyFlags));
  errors=0; written=0L;

  for (;;)
  {
#ifndef HAVE_PREAD
    int error;
    writenbytes= (uint) -1;
    pthread_mutex_lock(&my_file_info[Filedes].mutex);
    error=(lseek(Filedes, offset, MY_SEEK_SET) != -1L &&
	   (writenbytes = (uint) write(Filedes, Buffer, Count)) == Count);
    pthread_mutex_unlock(&my_file_info[Filedes].mutex);
    if (error)
      break;
#else
    if ((writenbytes = (uint) pwrite(Filedes, Buffer, Count,offset)) == Count)
      break;
#endif
    if ((int) writenbytes != -1)
    {					/* Safegueard */
      written+=writenbytes;
      Buffer+=writenbytes;
      Count-=writenbytes;
      offset+=writenbytes;
    }
    my_errno=errno;
    DBUG_PRINT("error",("Write only %d bytes",writenbytes));
#ifndef NO_BACKGROUND
#ifdef THREAD
    if (my_thread_var->abort)
      MyFlags&= ~ MY_WAIT_IF_FULL;		/* End if aborted by user */
#endif
    if ((my_errno == ENOSPC || my_errno == EDQUOT) &&
        (MyFlags & MY_WAIT_IF_FULL))
    {
      wait_for_free_space(my_filename(Filedes), errors);
      errors++;
      continue;
    }
    if ((writenbytes > 0 && (uint) writenbytes != (uint) -1) ||
        my_errno == EINTR)
      continue;					/* Retry */
#endif
    if (MyFlags & (MY_NABP | MY_FNABP))
    {
      if (MyFlags & (MY_WME | MY_FAE | MY_FNABP))
      {
	my_error(EE_WRITE, MYF(ME_BELL | ME_WAITTANG),
		 my_filename(Filedes),my_errno);
      }
      DBUG_RETURN(MY_FILE_ERROR);		/* Error on read */
    }
    else
      break;					/* Return bytes written */
  }
  if (MyFlags & (MY_NABP | MY_FNABP))
    DBUG_RETURN(0);			/* Want only errors */
  DBUG_RETURN(writenbytes+written); /* purecov: inspected */
} /* my_pwrite */

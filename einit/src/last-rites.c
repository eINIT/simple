/*
 *  last-rites.c
 *  einit
 *
 *  make REALLY sure that EVERYTHING is unmounted.
 *
 *  Created by Magnus Deininger on 21/08/2007.
 *  Copyright 2006, 2007 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2007, Magnus Deininger
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
    * Neither the name of the project nor the names of its contributors may be
	  used to endorse or promote products derived from this software without
	  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef LINUX

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include <einit/config.h>
#include <einit/utility.h>

#include <sys/reboot.h>
#include <linux/reboot.h>
#include <syscall.h>
#include <sys/syscall.h>
#include <linux/unistd.h> 
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <sys/mount.h>

#include <ctype.h>

// let's be serious, /sbin is gonna exist
#define LRTMPPATH "/sbin"

#if 0
#define kill(a,b) 1
#endif

int unmount_everything() {
 int errors = 0;
 FILE *fp;

 if ((fp = fopen ("/proc/mounts", "r"))) {
  char buffer[BUFFERSIZE];
  errno = 0;
  while (!errno) {
   if (!fgets (buffer, BUFFERSIZE, fp)) {
    switch (errno) {
     case EINTR:
     case EAGAIN:
      errno = 0;
      break;
     case 0:
      goto done_parsing_file;
     default:
      perror("fgets() failed.");
      goto done_parsing_file;
    }
   } else if (buffer[0] != '#') {
    strtrim (buffer);

    if (buffer[0]) {
     char *cur = estrdup (buffer);
     char *scur = cur;
     uint32_t icur = 0;

     char *fs_spec = NULL;
     char *fs_file = NULL;
     char *fs_vfstype = NULL;
     char *fs_mntops = NULL;
     int fs_freq = 0;
     int fs_passno = 0;

     strtrim (cur);
     for (; *cur; cur++) {
      if (isspace (*cur)) {
       *cur = 0;
       icur++;
       switch (icur) {
        case 1: fs_spec = scur; break;
        case 2: fs_file = scur; break;
        case 3: fs_vfstype = scur; break;
        case 4: fs_mntops = scur; break;
        case 5: fs_freq = (int) strtol(scur, (char **)NULL, 10); break;
        case 6: fs_passno = (int) strtol(scur, (char **)NULL, 10); break;
       }
       scur = cur+1;
       strtrim (scur);
      }
     }
     if (cur != scur) {
      icur++;
      switch (icur) {
       case 1: fs_spec = scur; break;
       case 2: fs_file = scur; break;
       case 3: fs_vfstype = scur; break;
       case 4: fs_mntops = scur; break;
       case 5: fs_freq = (int) strtol(scur, (char **)NULL, 10); break;
       case 6: fs_passno = (int) strtol(scur, (char **)NULL, 10); break;
      }
     }

     if (fs_spec && strstr (fs_file, "/old") == fs_file) {
      if (umount (fs_file) && umount2(fs_file, MNT_FORCE)) {
       fprintf (stderr, "couldn't unmount %s\n", fs_file);
       errors++;

       if (fs_spec && fs_file && fs_vfstype && (mount(fs_spec, fs_file, fs_vfstype, MS_REMOUNT | MS_RDONLY, ""))) {
        fprintf (stderr, "couldn't remount %s either\n", fs_file);
       }

#ifdef MNT_EXPIRE
       /* can't hurt to try this one */
       umount2(fs_file, MNT_EXPIRE);
       umount2(fs_file, MNT_EXPIRE);
#endif
      }
     }

     errno = 0;
    }
   }
  }
  done_parsing_file:
  fclose (fp);
 }

 return errors;
}

void kill_everything() {
 DIR *d = opendir("/proc");

 if (d) {
  struct dirent *e;

  while ((e = readdir(d))) {
   if (e->d_name && e->d_name[0]) {
    pid_t pidtokill = atoi (e->d_name);

    if ((pidtokill > 0) && (pidtokill != 1) && (pidtokill != getpid())) {
     if (kill (pidtokill, SIGKILL)) fprintf (stderr, "couldn't send SIGKILL to %i\n", pidtokill);
    }
   }
  }

  closedir (d);
 }
}

int lastrites () {
 if (mount ("lastrites", LRTMPPATH, "tmpfs", 0, "")) {
  perror ("couldn't mount my tmpfs at " LRTMPPATH);
//  return -1;
 }

 if (mkdir (LRTMPPATH "/old", 0777)) perror ("couldn't mkdir '" LRTMPPATH "/old'");
 if (mkdir (LRTMPPATH "/proc", 0777)) perror ("couldn't mkdir '" LRTMPPATH "/proc'");
// if (mkdir (LRTMPPATH "/dev", 0777)) perror ("couldn't mkdir '" LRTMPPATH "/dev'");

 if (mount ("lastrites-proc", LRTMPPATH "/proc", "proc", 0, "")) perror ("couldn't mount another 'proc' at '" LRTMPPATH "/proc'");
// if (mount ("/dev", LRTMPPATH "/dev", "", MS_BIND, "")) perror ("couldn't bind another 'dev'");

 if (pivot_root (LRTMPPATH, LRTMPPATH "/old")) perror ("couldn't pivot_root('" LRTMPPATH "', '" LRTMPPATH "/old')");

 chdir ("/");

 do {
  kill_everything();
 } while (unmount_everything());

 return 0;
}

int main(int argc, char **argv) {
 char action = argv[1] ? argv[1][0] : '?';

 fprintf (stderr, "\e[2J >> eINIT " EINIT_VERSION_LITERAL " | last rites <<\n"
   "###############################################################################\n");

 lastrites();

 switch (action) {
  case 'k':

#if defined(LINUX_REBOOT_MAGIC1) && defined (LINUX_REBOOT_MAGIC2) && defined (LINUX_REBOOT_CMD_KEXEC) && defined (__NR_reboot)
   syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_KEXEC, 0);
   fprintf (stderr, "whoops, looks like the kexec failed!\n");
#else
   fprintf (stderr, "no support for kexec?\n");
#endif

  case 'r':
   reboot (LINUX_REBOOT_CMD_RESTART);
   fprintf (stderr, "can't reboot?\n");

  case 'h':
   reboot (LINUX_REBOOT_CMD_POWER_OFF);
   fprintf (stderr, "can't shut down?\n");

  default:
   fprintf (stderr, "exiting... this is bad.\n");
   exit (EXIT_FAILURE);
   break;
 }

 return 0;
}

#else

#include <stdio.h>

int main(int argc, char **argv) {
 fprintf (stderr, "last-rites (%c), only supported on linux, sorry.\n", argv[1] ? argv[1][0] : '?');

 return 0;
}

#endif

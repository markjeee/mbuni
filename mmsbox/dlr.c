/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBox Dlr: Dlr storage for MMSBox
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <sys/file.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <dlfcn.h>

#ifdef SunOS
#include <fcntl.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mmsbox.h"
#include "mms_queue.h"

#define MAXTRIES 10
static int dlr_entry_fname(char *msgid, char *rtype, Octstr *mmc_gid, Octstr **efname)
{
     char d1[2], d2[3], fbuf[512], *p;
     unsigned long h = _mshash(msgid);
     Octstr *t, *s;
     int i, fd;

     /* Make toplevel dir. */
     d1[0] = _TT[h%_TTSIZE];
     d1[1] = '\0';
     
     /* Then lower level. */
     h /= _TTSIZE;
     d2[0] = _TT[h%_TTSIZE];
     h /= _TTSIZE;     
     d2[1] = _TT[h%_TTSIZE];
     d2[2] = '\0';
     
     /* Try and create the next level dir (first level was created by root_init) */
     sprintf(fbuf, "%s/%s/%s", octstr_get_cstr(dlr_dir), d1, d2);
     if (mkdir(fbuf, 
               S_IRWXU|S_IRWXG) < 0 && 
         errno != EEXIST) {
	  mms_error(0, "mmsbox", NULL,  "failed to create dir [%s] "
                "while initialising dlr dir for %s: %s!",
                fbuf, msgid, strerror(errno));
          return -1;
     }
     
     t = octstr_format("%S-%s_%s", mmc_gid, msgid, rtype); /* Put mmc id into name. */

     octstr_replace(t, octstr_imm("/"), octstr_imm("$")); /* XXX safe in all cases?? */
     s  = octstr_format("%s/%S", fbuf, t);
     octstr_destroy(t);
     

     p = octstr_get_cstr(s);
     i = 0;
     do          
          if ((fd = open(p, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)) < 0) {
               mms_error(0,  "mmsbox", NULL, "Failed to open DLR URL store  file [%s], error: %s!",
                     p, strerror(errno));
               break;
          }  else if (mm_lockfile(fd, p, 1) != 0) {          
               unlock_and_close(fd);
               fd = -1;
          }
     while (i++ < MAXTRIES && fd < 0);

     if (efname)
	  *efname = s;  
     else
	  octstr_destroy(s);
     
     return fd;
}

void mms_dlr_url_put(Octstr *msgid, char *rtype, Octstr *mmc_gid, Octstr *dlr_url, Octstr *transid)
{
     int fd = dlr_entry_fname(octstr_get_cstr(msgid), rtype, mmc_gid, NULL);

     if (fd >= 0) {
	  Octstr *x = octstr_format("%S %S", transid ? transid : octstr_imm("x"), dlr_url); /* better have no spaces in transid! */
	  octstr_write_data(x, fd, 0);
	  unlock_and_close(fd);
	  octstr_destroy(x);
     }     
}

int mms_dlr_url_get(Octstr *msgid,  char *rtype, Octstr *mmc_gid, Octstr **dlr_url, Octstr **transid)
{
     int fd = dlr_entry_fname(octstr_get_cstr(msgid), rtype, mmc_gid, NULL);
     FILE *f;
     
     if (fd >= 0 && (f =  fdopen(fd, "r+")) != NULL) {	   
	  Octstr *s  = octstr_read_pipe(f);
	  int i, ret;
	  
	  unlock_and_fclose(f);
	  if (s && octstr_len(s) == 0) {
	       ret = -1;
	  } else if ((i = octstr_search_char(s, ' ', 0)) >= 0) {
	       *transid = octstr_copy(s, 0, i);
	       *dlr_url = octstr_copy(s, i+1, octstr_len(s));
	       ret = 0;
	  } else 
	       ret = -1;
	  octstr_destroy(s);
	  return ret;
     } else if (fd >= 0)
	  unlock_and_close(fd);
     return -1;
}

void mms_dlr_url_remove(Octstr *msgid, char *rtype, Octstr *mmc_gid)
{
     Octstr *fname = NULL;
     int fd = dlr_entry_fname(octstr_get_cstr(msgid), rtype, mmc_gid, &fname);
     
     if (fname) {
	  unlink(octstr_get_cstr(fname));
	  octstr_destroy(fname);
     }
     if (fd >= 0)
	  unlock_and_close(fd);   

}

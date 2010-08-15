/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni MMBox implementation
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include "mms_mmbox.h"
#include "mms_util.h"
#include "gwlib/log.h"
#include "gwlib/accesslog.h"

#define MAXTRIES 10
#define MDF 'd'
#define MTF 't'
#define IDXFILE "00Index"

#define ITEM_ADD 1
#define ITEM_DEL 2
#define ITEM_MOD 3



/* Initialise the root of the mmbox. Should be called once from load settings. */
int mmbox_root_init(char *mmbox_root)
{
     int i, ret;
     if ((ret = mkdir(mmbox_root, 
		      S_IRWXU|S_IRWXG)) < 0 && 
	 errno != EEXIST)
	  return errno;
     for (i = 0; _TT[i]; i++) {
	  char fbuf[256];

	  sprintf(fbuf, "%.128s/%c", mmbox_root, _TT[i]);	  
	  if (mkdir(fbuf, 
		    S_IRWXU|S_IRWXG) < 0 && 
	      errno != EEXIST)
	       return errno;
     }     
     srandom(time(NULL)); /* we need rands below...*/

     return 0;
}

/* Initialise the mmbox home of the user/msisdn.
 * structure of mmbox is: 
 * - index file [IDXFILE]
 * - directories a - z 0 - 9, which each can contain directories with two-character 
 *   names
 * on init we only create the directory itself. internal 
 * directory structure and index will be made by first message put in.
 * return user dir, or NULL if something went wrong.
 */
static Octstr *user_mmbox_dir(char *mmbox_root, char *userid)
{

     unsigned long h = _mshash(userid);
     char d1[2], d2[3], fbuf[512];
     Octstr *t, *s;

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
     sprintf(fbuf, "%.128s/%s/%s", mmbox_root, d1, d2);
     if (mkdir(fbuf, 
	       S_IRWXU|S_IRWXG) < 0 && 
	 errno != EEXIST) {
	  mms_error(0, "mmbox", NULL, "Failed to create dir [%s] "
		"while initialising mmbox for %s: %s!",
		fbuf, userid, strerror(errno));
	  return NULL;
     }
     
     t = octstr_create(userid);
     octstr_replace(t, octstr_imm("/"), octstr_imm("$")); /* XXX safe in all cases?? */
     s  = octstr_format("%s/%S", fbuf, t);
     octstr_destroy(t);

     if (mkdir(octstr_get_cstr(s), 
	       S_IRWXU|S_IRWXG) < 0 && 
	 errno != EEXIST) {
	  mms_error(0, "mmbox", NULL, "Failed to create dir [%s] "
		"while initialising mmbox for %s: %s!",
		octstr_get_cstr(s), userid, strerror(errno));
	  octstr_destroy(s);
	  return NULL;
     }
     return s;
}

/* Makes a file name in the nested directory structure, where we can store 
 * data. Makes a number of tries -- similar to mkqf in queue module. 
 */
static int mkdf(char df[64], char *mmbox_home)
{

     int i = 0, fd = -1;
     static int ect;
     
     if (!mmbox_home)
	  gw_panic(0, "Mmbox directory passed as null!");
     
     do {	 	  
	  char d1[2], d2[3];
	  Octstr *tmp;
	  char *ctmp;
	  
	  d1[0] = _TT[random() % _TTSIZE];
	  d1[1] = '\0';
	  
	  /* Make first level. */
	  tmp = octstr_format("%.128s/%s", mmbox_home, d1);	  
	  if (mkdir(octstr_get_cstr(tmp), 
		    S_IRWXU|S_IRWXG) < 0 && 
	      errno != EEXIST) {
	       mms_error(0, "mmbox", NULL, "failed to create dir [%s] "
		     " in mmbox home %s: %s!",
		     octstr_get_cstr(tmp), mmbox_home, strerror(errno));
	       octstr_destroy(tmp);
	       return -1;
	  }

	  octstr_destroy(tmp);

	  d2[0] = _TT[random() % _TTSIZE];
	  d2[1] = _TT[random() % _TTSIZE];
	  d2[2] = '\0';

	  /* Make second level. */
	  tmp = octstr_format("%.128s/%s/%s", mmbox_home, d1,d2);
	  
	  if (mkdir(octstr_get_cstr(tmp), 
		    S_IRWXU|S_IRWXG) < 0 && 
	      errno != EEXIST) {
	       mms_error(0, "mmbox", NULL, "Failed to create dir [%s] "
		     " in mmbox home %s: %s!",
		     octstr_get_cstr(tmp), mmbox_home, strerror(errno));
	       octstr_destroy(tmp);
	       return -1;
	  }
	  octstr_destroy(tmp);
	  
	  /* use df[] to store candidate so when we hit success it is already there...*/
	  sprintf(df, "%s/%s/%cf%ld.%d.x%d%ld", 
			      d1,d2, MDF, 
			      time(NULL), 
			      ++ect, getpid(), random() % 100);
	  tmp = octstr_format("%s/%s", mmbox_home, df);
	  ctmp = octstr_get_cstr(tmp);
	  fd = open(ctmp, O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	  if (fd >= 0 && 
	      mm_lockfile(fd,ctmp,1) != 0) {
	       unlink(ctmp);
	       unlock_and_close(fd);
	       fd = -1;
	  }
	  octstr_destroy(tmp);
	  
     } while (i++ < MAXTRIES && fd < 0);

     return fd;
}

static int open_mmbox_index(char *mmbox_dir, int shouldblock)
{
     char fbuf[256];
     int i, fd;
     
     sprintf(fbuf, "%s/%s", mmbox_dir, IDXFILE);

     i = 0;
     do 	 
	  if ((fd = open(fbuf, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)) < 0) {
	       mms_error(0, "mmbox", NULL, "Failed to open mmbox index file [%s], error: %s!",
		     fbuf, strerror(errno));
	       break;
	  }  else if (mm_lockfile(fd, fbuf, shouldblock) != 0) {	  
	       unlock_and_close(fd);
	       fd = -1;
	  }
     while (i++ < MAXTRIES && fd < 0);
     
     return fd;
}

static Octstr *linearise_string_list(List *l, char *sep)
{
     int i, n;

     Octstr *s = octstr_create("");

     for (i = 0, n = gwlist_len(l); i < n; i++) {
	  Octstr *p = gwlist_get(l,i);
	  if (p)
	       octstr_format_append(s, "%s%S", (i == 0) ? "" : sep, p);
     }
     return s;
}

static List *parse_string_list(char *buf)
{
     int i = 0;
     char sbuf[128], *p = buf;
     List *l = gwlist_create();
     
     while (sscanf(p, "%s%n", sbuf, &i) > 0) {
	  gwlist_append(l, octstr_create(sbuf));
	  p += i;
     }
     
     return l;
}

static char *skip_space(char *s)
{
     while (*s && isspace(*s))
	  s++;
     return s;     
}

/* Format of Index file:
 * each message is described by a single line:
 * df state flag1 flag2 flag3 ...
 */
static int update_mmbox_index(int fd, char *mmbox_dir, int cmd, 
			      Octstr *df, Octstr *state, List *flags, long msgsize)
{
     char fbuf[256], linbuf[1024];
     int tempfd;
     FILE *fp;
     
     /* Make a temp file. */
     
     sprintf(fbuf, "%.128s/t%s.%ld.%ld",
	     mmbox_dir, IDXFILE, time(NULL), random() % 1000);

     tempfd = open(fbuf, 
		   O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
     if (tempfd < 0 ) { 
	  mms_error(0, "mmbox", NULL,"Failed to open temp file %s: error = %s\n", 
		fbuf, strerror(errno));
	  
	  goto done;
     } else if (mm_lockfile(tempfd, fbuf, 0) != 0) { /* Lock it. */
	  mms_error(0, "mmbox", NULL,"Failed lock  temp file %s: error = %s\n", 
		     fbuf, strerror(errno));

	  unlock_and_close(tempfd);
	  tempfd = -1;
	  goto done;
	  
     }
     fp = fdopen(fd, "r");
     
     if (!fp) {
	  mms_error(0, "mmbox", NULL,"Failed fdopen on tempfd, file %s: error = %s\n",
		
		fbuf, strerror(errno));
		  
	  unlock_and_close(tempfd);
	  tempfd = -1;
	  goto done;
     }
     
     while (fgets(linbuf, sizeof linbuf, fp) != NULL) {
	  char idx[128], xstate[32];
	  Octstr *outs = NULL;
	  int i;
	  int size;
	  
	  sscanf(linbuf, "%s %s %d%n", idx, xstate, &size, &i);
	  
	  if (df && octstr_str_compare(df, idx) == 0)
	       if (cmd == ITEM_DEL || cmd == ITEM_ADD)
		    goto loop; /* Skip it. */
	       else  { /* MOD. */
		    Octstr *p = linearise_string_list(flags, " ");
		    outs = octstr_format("%S %S %d %S\n", df, state, msgsize, p);
		    octstr_destroy(p);
	       }
	  else { /* Copy out as-is */
	       char *p = skip_space(linbuf + i);
	       outs = octstr_format("%s %s %d %s%s",
				    idx, xstate, 
				    size,
				    p,
				    (strchr(p, '\n') != NULL ? "" : "\n"));
	  }
     loop:
	  if (outs) {
	       if (octstr_len(outs) > 0) 
		    octstr_write_to_socket(tempfd, outs);	       
	       octstr_destroy(outs);
	  }
     }     

     if (cmd == ITEM_ADD) { /* Finally, for ADD, just add it. */
	  Octstr *s, *p = linearise_string_list(flags, " ");
	  s = octstr_format("%S %S %d %S\n", df, state, msgsize, p);
	  octstr_destroy(p);	  
	  octstr_write_to_socket(tempfd, s);	       
	  octstr_destroy(s);	  
     }
     fsync(tempfd);     

     sprintf(linbuf, "%.128s/%s",
	     mmbox_dir, IDXFILE);
     rename(fbuf, linbuf);

     fclose(fp);
 done:

     return tempfd;
}

static List *make_mm_flags(List *oflags, List *flag_cmds)
{
     List *l = oflags ? oflags : gwlist_create();
     int i, n;

     for (i = 0, n = gwlist_len(l); i < n; i++) { /* cleanup list. */
	  Octstr *x = gwlist_get(l,i);
	  int ch = octstr_get_char(x, 0);
	  
	  if (ch == '+' || ch == '-' || ch == '/')
	       octstr_delete(x,0,1);
     }
     
     for (i = 0, n = (flag_cmds ? gwlist_len(flag_cmds) : 0); i<n; i++) {
	  Octstr *x = gwlist_get(flag_cmds,i);
	  int ch = octstr_get_char(x, 0);
	  char *s = octstr_get_cstr(x);
	  int j, m, cmd;
	  
	  if (ch == '+' || ch == '-' || ch == '/') {
	       s++;
	       cmd = ch;
	  } else 
	       cmd = '+';
	  
	  /* Find it in original. If existent, remove it. */
	  for (j = 0, m = gwlist_len(l); j < m; j++) 
	       if (octstr_str_compare(gwlist_get(l,j),s) == 0)  { 
		    Octstr *y = gwlist_get(l,j);
		    gwlist_delete(l,j,1);
		    octstr_destroy(y);
		    j--;
		    m--;
	       } 
	  
	  if (cmd == '+' || cmd == '/')
	       gwlist_append(l, octstr_create(s));
     }     


     return l;
}

Octstr *mms_mmbox_addmsg(char *mmbox_root, char *user, MmsMsg *msg, List *flag_cmds, Octstr *dfltstate)
{
     int ifd = -1, nifd, dfd = -1;
     char df[128];
     Octstr *home = user_mmbox_dir(mmbox_root,user);
     Octstr *s = octstr_create(""), *sdf = NULL;
     List *flags = NULL;
     Octstr *state = NULL;
     int msize;
     
     if (!home)
	  goto done;
     ifd = open_mmbox_index(octstr_get_cstr(home),1);
     
     if (ifd < 0) 
	  goto done;
     
     if ((dfd = mkdf(df, octstr_get_cstr(home))) < 0) {
	  mms_error(0, "mmbox", NULL, "failed to create data file, home=%s - %s!", 
		octstr_get_cstr(home), strerror(errno));
	  goto done;
     }
     
     state = mms_get_header_value(msg, octstr_imm("X-Mms-MM-State"));
     flags = make_mm_flags(mms_get_header_values(msg, octstr_imm("X-Mms-MM-Flags")), flag_cmds);
     
     if (state == NULL)
	  state = dfltstate ? octstr_duplicate(dfltstate) : octstr_create("Sent");
     
     mms_replace_header_values(msg, "X-Mms-MM-Flags", flags);
     mms_replace_header_value(msg, "X-Mms-MM-State", octstr_get_cstr(state));
     
     s = mms_tobinary(msg);   
     msize = octstr_len(s);
     
     octstr_write_to_socket(dfd, s);	       
     sdf = octstr_create(df);
     
     if ((nifd = update_mmbox_index(ifd, octstr_get_cstr(home), ITEM_ADD, sdf, state, flags, msize)) < 0 ) {
	  char fbuf[256];	  
	  sprintf(fbuf, "%s/%s", octstr_get_cstr(home), df);
	  unlink(fbuf);	  
	  octstr_destroy(sdf);
	  sdf = NULL;
	  goto done;
     }
     
     ifd = nifd;
     octstr_replace(sdf, octstr_imm("/"), octstr_imm("-"));
 done:
     if (dfd > 0) 
	  unlock_and_close(dfd);
     if (ifd > 0) 
	  unlock_and_close(ifd);

     octstr_destroy(s);
     octstr_destroy(home);
     octstr_destroy(state);
     gwlist_destroy(flags, (gwlist_item_destructor_t *)octstr_destroy);
     
     return sdf;
}

int mms_mmbox_modmsg(char *mmbox_root, char *user, Octstr *msgref, 
		      Octstr *state, List *flag_cmds)
{
     Octstr *sdf = octstr_duplicate(msgref);
     Octstr *fname = NULL, *ftmp = NULL; 
     Octstr *home = user_mmbox_dir(mmbox_root,user);
     Octstr *s = NULL;
     List  *flags = NULL;
     Octstr *nstate = NULL;
     
     int ifd = -1, nifd, tmpfd = -1;
     MmsMsg *m  = NULL;
     int res = -1;
     int msize;
     
     octstr_replace(sdf, octstr_imm("-"), octstr_imm("/"));

     if (!home)
	  goto done;

     ifd = open_mmbox_index(octstr_get_cstr(home),1);
     
     if (ifd < 0) 
	  goto done;

     fname = octstr_format("%S/%S", home, sdf);
     s  = octstr_read_file(octstr_get_cstr(fname));
     
     if ( s == NULL || octstr_len(s) == 0) {
	  mms_error(0, "mmbox", NULL, "Failed to read data file [%s] - %s!",
		octstr_get_cstr(fname), strerror(errno));
	  goto done;
     }
     
     m = mms_frombinary(s, octstr_imm("anon@anon"));     

     if (!m) {
	  mms_error(0, "mmbox", NULL, "Failed to read data file [%s]!",
		octstr_get_cstr(fname));
	  goto done;
     }

     if (state == NULL)
	  nstate = mms_get_header_value(m, octstr_imm("X-Mms-MM-State"));
     else {
	  nstate = octstr_duplicate(state);
	  mms_replace_header_value(m, "X-Mms-MM-State", octstr_get_cstr(nstate));
     }
     
     flags = mms_get_header_values(m, octstr_imm("X-Mms-MM-Flags"));     
     flags = make_mm_flags(flags, flag_cmds);     
     mms_replace_header_values(m, "X-Mms-MM-Flags", flags);


     ftmp = octstr_format("%S.%ld.%d", fname, time(NULL), getpid());
     tmpfd =  open(octstr_get_cstr(ftmp), O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
     
     if (tmpfd < 0) 
	  goto done;
     
     s = mms_tobinary(m);
     msize = octstr_len(s);

     octstr_write_to_socket(tmpfd, s);    

     rename(octstr_get_cstr(ftmp), octstr_get_cstr(fname));
     close(tmpfd);

     if ((nifd = update_mmbox_index(ifd, octstr_get_cstr(home), ITEM_MOD, sdf, nstate, flags, msize)) < 0) {
	  /* Not good, we wrote but could not update the index file. scream. */
	  mms_error(0, "mmbox", NULL, "Failed to update index file, home is %s!",
		octstr_get_cstr(home));
	  goto done;
     }     
     ifd = nifd;
     res = 0;

 done:
     if (ifd > 0) 
	  close(ifd);

     if (fname)
	  octstr_destroy(fname);
     
     if (ftmp)
	  octstr_destroy(ftmp);

     if (sdf)
	  octstr_destroy(sdf);

     if (s)
	  octstr_destroy(s);
     if (home) 
	  octstr_destroy(home);
     if (nstate)
	  octstr_destroy(nstate);
     if (flags)
	  gwlist_destroy(flags, (gwlist_item_destructor_t *)octstr_destroy);     
     if (m)
	  mms_destroy(m);
     return res;
}


int mms_mmbox_delmsg(char *mmbox_root, char *user, Octstr *msgref)
{
 
     Octstr *sdf = octstr_duplicate(msgref);
     Octstr *fname = NULL, *home = user_mmbox_dir(mmbox_root,user);
     Octstr *s = NULL;
     int res = -1;
     int ifd = -1, nifd;
     
     
     octstr_replace(sdf, octstr_imm("-"), octstr_imm("/"));

     if (!home)
	  goto done;

     ifd = open_mmbox_index(octstr_get_cstr(home),1);
     
     if (ifd < 0) 
	  goto done;

     fname = octstr_format("%S/%S", home, sdf);

     if ((nifd = update_mmbox_index(ifd, octstr_get_cstr(home), ITEM_DEL, sdf, NULL, NULL,0)) < 0) {

	  mms_error(0, "mmbox", NULL,"Failed to update index file, home is %s!",
		octstr_get_cstr(home));
	  goto done;
     }     
     unlink(octstr_get_cstr(fname));
     ifd = nifd;
     
     res = 0;
 done:
     if (ifd > 0) 
	  close(ifd);

     if (fname)
	  octstr_destroy(fname);

     if (s)
	  octstr_destroy(s);
     if (home) 
	  octstr_destroy(home);

     return 0;
}

static int string_in_list(Octstr *s, List *l)
{
     int i, n;
     
     for (i = 0, n = gwlist_len(l); i<n; i++) {
	  Octstr *x = gwlist_get(l,i);
	  char *p = octstr_get_cstr(x);
	  
	  if (p[0] == '+' ||
	      p[0] == '/' || p[0] == '-')
	       p++;	  
	  if (octstr_str_compare(s, p) == 0)
	       return 1;
     }
     return 0;
}

static int _x_octstr_str_compare(Octstr *s, char *p)
{
     return (octstr_str_compare(s,p) == 0);
}

static void replace_slash(char s[])
{
     while (*s) 
	  if (*s == '/')
	       *s++ = '-';
	  else 
	       s++;     
}

List *mms_mmbox_search(char *mmbox_root, char *user,
		       List *state, List *flag_cmds, int start, int limit, 
		       List *msgrefs)
{
     int tmpfd = -1;
     FILE *fp = NULL;
     char linbuf[1024];

     Octstr *home = user_mmbox_dir(mmbox_root,user);
     List *flags = NULL;
     List *dflist = NULL;

     int ifd = -1;
     int ct;
     
     ifd = open_mmbox_index(octstr_get_cstr(home),1);
     
     if (ifd < 0) 
	  goto done;

     if ((tmpfd = dup(ifd)) < 0 || 
	 (fp = fdopen(tmpfd, "r")) == NULL) {
	  mms_error(0, "mmbox", NULL, "%s Failed to dup descriptor for index "
		"file, fp = %p: error = %s\n", octstr_get_cstr(home), 
		fp, strerror(errno));	  
	  goto done;
     }

     flags = make_mm_flags(NULL, flag_cmds);
     
     ct = 1;
     dflist = gwlist_create();
     while (fgets(linbuf, sizeof linbuf, fp) != NULL) {
	  char idx[128], xstate[32];
	  List *xflags = NULL;
	  int i, size;

	  int match = (!state && (!msgrefs || gwlist_len(msgrefs) == 0) && (!xflags || gwlist_len(xflags) == 0));
	  
	  sscanf(linbuf, "%s %s %d%n", idx, xstate, &size, &i);
	  
	  /* search: by id list if given, by  states if given, by flags if given */
	  if (!match && state && gwlist_search(state, xstate, 
				   (gwlist_item_matches_t *)_x_octstr_str_compare) != NULL) 
	       match = 1;
	  
	  /* For the rest we only match if nothing else matched. Save time */
	  replace_slash(idx);
	  if (!match && msgrefs &&  	       
	      gwlist_search(msgrefs, idx, 
			  (gwlist_item_matches_t *)_x_octstr_str_compare) != NULL)
	       match = 1;
	  
	  if (!match &&
	      flag_cmds  &&
	       ((xflags = parse_string_list(linbuf + i)) != NULL && 
		gwlist_search(xflags, flags, (gwlist_item_matches_t *)string_in_list) != NULL))
	       match = 1;

	  if (match && ct >= start && gwlist_len(dflist) <= limit) {
		    Octstr *x = octstr_create(idx);
		    /* octstr_replace(x, octstr_imm("/"), octstr_imm("-")); */
		    gwlist_append(dflist, x);	       
	  }
	  ct++;
	  if (xflags)
	       gwlist_destroy(xflags, (gwlist_item_destructor_t *)octstr_destroy);     	  
     }     

 done:
     if (fp) 
	  fclose(fp);
     else if (tmpfd)
	  close(tmpfd);
     
     if (ifd > 0)
	  close(ifd);

     if (flags)
	  gwlist_destroy(flags, (gwlist_item_destructor_t *)octstr_destroy);     

     if (home) 
	  octstr_destroy(home);

     return dflist;
}

MmsMsg *mms_mmbox_get(char *mmbox_root, char *user,  Octstr *msgref, unsigned long *msize)
{
     Octstr *sdf = octstr_duplicate(msgref);
     Octstr *fname = NULL, *home = user_mmbox_dir(mmbox_root,user);
     Octstr *s = NULL;
     int ifd = -1;
     MmsMsg *m = NULL;
     
     octstr_replace(sdf, octstr_imm("-"), octstr_imm("/"));

     if (!home)
	  goto done;

     ifd = open_mmbox_index(octstr_get_cstr(home),1); /* Grab a lock on the index file. */
     
     if (ifd < 0) 
	  goto done;

     fname = octstr_format("%S/%S", home, sdf);
     s  = octstr_read_file(octstr_get_cstr(fname));
     
     if (s) {
	  if (msize) 
	       *msize = octstr_len(s);
	  m = mms_frombinary(s, octstr_imm("anon@anon"));     
     } else if (msize) 
	  *msize = 0;
 done:
     if (ifd > 0) 
	  close(ifd);

     if (fname)
	  octstr_destroy(fname);

     if (s)
	  octstr_destroy(s);
     if (home) 
	  octstr_destroy(home);

     return m;
     
}

int mms_mmbox_count(char *mmbox_root, char *user,  unsigned long *msgcount, unsigned long *byte_count)
{

     int tmpfd = -1;
     FILE *fp = NULL;
     char linbuf[1024];
     int ret = -1;
     Octstr *home = user_mmbox_dir(mmbox_root,user);
     int ifd = -1;

     
     ifd = open_mmbox_index(octstr_get_cstr(home),1);
     
     if (ifd < 0) 
	  goto done;

     if ((tmpfd = dup(ifd)) < 0 || 
	 (fp = fdopen(tmpfd, "r")) == NULL) {
	  mms_error(0, "mmbox", NULL, "%s Failed to dup descriptor for index "
		"file, fp = %p: error = %s\n", octstr_get_cstr(home), 
		fp, strerror(errno));	  
	  goto done;
     }

     *msgcount = 0;
     *byte_count = 0;

     while (fgets(linbuf, sizeof linbuf, fp) != NULL) {
	  int size = 0;	 

	  sscanf(linbuf, "%*s %*s %d", &size);	  
	  ++*msgcount;
	  *byte_count = *byte_count + size;
     }     
     ret = 0;

 done:
     if (fp) 
	  fclose(fp);
     else if (tmpfd)
	  close(tmpfd);
     
     if (ifd > 0)
	  close(ifd);

     return ret;
}

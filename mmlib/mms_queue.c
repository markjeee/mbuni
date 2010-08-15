/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Queue management functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#ifdef SunOS
#include <strings.h>
#endif
#include <fcntl.h>
#include <dirent.h>

#include "mms_queue.h"
#include "gwlib/log.h"
#include "gwlib/accesslog.h"

#define MQF 'q'
#define MDF 'd'
#define MTF 't'

#define DIR_SEP '/'
#define DIR_SEP_S "/"


struct qfile_t {            /* Name of the queue file, pointer to it (locked). DO NOT USE THESE! */
     char name[QFNAMEMAX];   /* Name of the file. */
     char dir[QFNAMEMAX];    /* Directory in which file is .*/
     char subdir[64];        /* and the sub-directory. */
     char _pad[16];
     int fd;
};


static char sdir[QFNAMEMAX*2+1]; /* top-level storage directory. */
static int inited;
static int mms_init_queue_module(Octstr *storage_dir,  char *unused, int max_threads)
{
     gw_assert(inited==0);     
     gw_assert(storage_dir);

     strncpy(sdir, octstr_get_cstr(storage_dir), -1 + sizeof sdir);
     inited = 1;
     return 0;
}

static Octstr *mms_init_queue_dir(char *qdir, int *error)
{
     Octstr *dir;
     int i, ret;
     char fbuf[512], *xqdir;
     
     gw_assert(inited);
     gw_assert(qdir);
     
     if (error == NULL) error = &ret;
     *error = 0;
     
     dir = octstr_format("%s%c%s", sdir, DIR_SEP, qdir);     
     octstr_strip_blanks(dir);
    
     /* Remove trailing slashes. */     
     for (i = octstr_len(dir) - 1; i >= 0; i--)
	  if (octstr_get_char(dir, i) != DIR_SEP)
	       break;
	  else
	       octstr_delete(dir, i,1);

     xqdir = octstr_get_cstr(dir);
     
     if ((ret = mkdir(xqdir, 
		      S_IRWXU|S_IRWXG)) < 0 && 
	 errno != EEXIST) {
	  *error = errno;
	  goto done;
     }
     
     for (i = 0; _TT[i]; i++) { /* initialise the top level only... */	 
	  sprintf(fbuf, "%.128s/%c", xqdir, _TT[i]);
	  if (mkdir(fbuf, 
		    S_IRWXU|S_IRWXG) < 0 && 
	      errno != EEXIST) {
	       *error = errno;
	       goto done;
	  }
     }

 done:
     if (*error == 0)
	  return dir;
     octstr_destroy(dir);
     return NULL;  
}

static int free_envelope(MmsEnvelope *e, int removefromqueue);
/* Queue file structure: 
 * - File consists of a series of lines, each line begins with a single letter, followed by 
 *   a parameter. Letters mean:
 * T - message type (full text string -- MMS message type.
 * I - message ID
 * i - source interface (MM1, MM4, etc.)
 * F - From address 
 * R - Recipient (the ones pending) for this message
 * z - Recipient (those who already received)
 * C - Time queue entry was created
 * L - Time of last delivery attempt
 * D - Time of (next) delivery attempt
 * X - Time of expiry of message
 * N - Number of delivery attempts so far
 * P - Proxy who sent it to us
 * p - Proxy through which this message shd be delivered (e.g. delivery report)
 * S - Message size
 * s - Message subject.
 * f - time of last content fetch 
 * t - user defined token.
 * b - billed amount.
 * r - whether delivery receipts are required or not.
 * M - Application specific data (string)
 * c - Message Class 
 * V - VASPID -- from VASP
 * v - vasid -- from VASP
 * U - url1 -- e.g. for delivery report
 * u - url2 -- e.g. for read report
 * H - generic headers associated with message (e.g. for passing to MMC)
 */


static int _putline(int fd, char *code, char buf[])
{
     Octstr *s = octstr_format("%s%s\n", code, buf);
     int res;
     
     res = octstr_write_to_socket(fd, s);
     octstr_destroy(s);
     return res;
}

static Octstr *xmake_qf(char realqf[], char subdir[])
{

     Octstr *res = octstr_format("%s%s", subdir, realqf); /* Make the queue identifier -- convert '/' to '-' */
     octstr_replace(res, octstr_imm(DIR_SEP_S), octstr_imm("-"));
     return res;
}

/* break down a qf into dir and sub-dir. subdir contains final '/'! */
static void get_subdir(char *qf, char subdir[64], char realqf[QFNAMEMAX])
{
     char *p = strrchr(qf, '-');
     
     if (p == NULL) {
	  strncpy(realqf, qf, QFNAMEMAX);
	  subdir[0] = '\0';
     } else {
	  int i, n;
	  strncpy(realqf, p + 1, QFNAMEMAX);
	  
	  n = (p+1) - qf;
	  strncpy(subdir, qf, n);
	  subdir[n] = 0;

	  for (i = 0; i<n; i++)
	       if (subdir[i] == '-')
		    subdir[i] = DIR_SEP;
     }	  
}
/* 
 * Attempt to read an envelope from queue file:
 * - opens and locks the file. 
 * - if the lock succeeds, check that file hasn't changed since opening. If it has
 *   return NULL (i.e. file is being processed elsewhere -- race condition), otherwise read it.
 * - If should block is 1, then does a potentially blocking attempt to lock the file.
 */
static MmsEnvelope *mms_queue_readenvelope(char *qf, char *mms_queuedir, int shouldblock)
{
     Octstr *fname;
     int fd;
     Octstr *qdata, *s;
     ParseContext *p;
     MmsEnvelope *e;
     int okfile = 0;
     char subdir[64];
     char realqf[QFNAMEMAX];
     char xqf[QFNAMEMAX+64];
     struct qfile_t *qfs;
     
     get_subdir(qf, subdir, realqf); /* break it down... */

     fname = octstr_format( "%.128s/%s%s", mms_queuedir, subdir, realqf);
     
     strncpy(xqf, octstr_get_cstr(fname), sizeof xqf);
     
     if ((fd = open(octstr_get_cstr(fname), O_RDWR)) < 0) {
	  octstr_destroy(fname);
	  return NULL;
     } else if (mm_lockfile(fd, octstr_get_cstr(fname), shouldblock) != 0) {
	  unlock_and_close(fd);
	  octstr_destroy(fname);
	  return NULL;	       
     }
     
     e = mms_queue_create_envelope(NULL, NULL, 
				   NULL, 
				   NULL, NULL, 
				   0, 0, 
				   NULL, 
				   NULL, NULL, 
				   NULL, NULL, 
				   NULL,
				   0, 
				   NULL, 
				   NULL,
				   qf,
				   NULL,
				   sizeof (struct qfile_t), NULL);
     qfs = e->qfs_data; 
     
     qfs->fd = fd;
     strncpy(qfs->name, realqf, sizeof qfs->name);
     strncpy(qfs->subdir, subdir, sizeof qfs->subdir);
     strncpy(qfs->dir, mms_queuedir, sizeof qfs->dir);

     qdata = octstr_read_file(octstr_get_cstr(fname));
     octstr_destroy(fname);
     if (qdata == NULL) 
	  qdata = octstr_imm("");
     p = parse_context_create(qdata);
     
     for (s = parse_get_line(p); s;  
	  s = parse_get_line(p)) {
	  char *line = octstr_get_cstr(s);
	  int ch = line[0];
	  char *res = line + 1;
	  char *ptmp;

	  switch (ch) {
	       Octstr *t;
	       MmsEnvelopeTo *to;
	  case 'T':
	       t = octstr_create(res);
	       e->msgtype = mms_string_to_message_type(t);
	       octstr_destroy(t);
	       if (e->msgtype < 0) {
		    e->msgtype = 0;
		    mms_error(0, "mms_queueread", NULL, "Unknown MMS message type (%s) in file %s, skipped!\n",
			  res, xqf);
	       }
	       break;
	  case 'I':
	       e->msgId = octstr_create(res);	       
	       break;
	  case 'i':
	       strncpy(e->src_interface, res, sizeof e->src_interface);
	       break;
	  case 'F':
	       e->from = octstr_create(res);
	       if (mms_validate_address(e->from) != 0) {
#if 0
		    mms_warning(0, "mms_queueread", NULL, "Mal-formed address [%s] in file %s! "
			    "Attempting fixup.", res, xqf);
#endif
		    _mms_fixup_address(&e->from, NULL, NULL, 1);
	       }
	       break;
	  case 'R':
	  case 'z':
	       t = octstr_create(res);
	       if (mms_validate_address(t) != 0) {
#if 0
		    mms_warning(0, "mms_queueread", NULL, "Mal-formed address [%s] in file %s! " 
			    "Attempting fixup.", res, xqf);
#endif
		    _mms_fixup_address(&t, NULL, NULL, 1);
	       }
	       to = gw_malloc(sizeof *to);
	       to->rcpt = t;
	       to->process = (ch == 'R') ? 1 : 0;	       
	       gwlist_append(e->to, to);
	       break;

	  case 'C':
	       e->created = atol(res);
	       break;
	  case 'c':
	       e->mclass = octstr_create(res);
	       break;
	  case 'L':
	       e->lasttry = atol(res);
	       break;
	  case 'D':
	       e->sendt = atol(res);
	       break;
	  case 'X':
	       e->expiryt = atol(res);
	       break;
	  case 'N':
	       e->attempts = atol(res);
	       break;
	  case 'P':
	       e->fromproxy = octstr_create(res);
	       break;
	  case 'M':
	       e->mdata = octstr_create(res);
	       break;
	  case 'p':
	       e->viaproxy = octstr_create(res);
	       break;
	  case 'S':
	       e->msize = atol(res);
	    break;
	  case 's':
	       e->subject = octstr_create(res);
	       break;	
	  case 't':
	       e->token = octstr_create(res);
	       break;
	  case 'f':
	       e->lastaccess = atol(res);
	       break;
	  case 'b':
	       e->bill.billed = 1;
	       e->bill.amt = atof(res);
	    break;
	  case 'r':
	       e->dlr = 1;
	       break;
	  case 'V':
	       e->vaspid = octstr_create(res);
	       break;
	  case 'v':
	       e->vasid = octstr_create(res);
	       break;

	  case 'U':
	       e->url1 = octstr_create(res);
	       break;

	  case 'u':
	       e->url2 = octstr_create(res);
	       break;
	  case 'H':
	       if (e->hdrs == NULL)
		    e->hdrs = http_create_empty_headers();
	       if ((ptmp = index(res, ':')) == NULL)
		    mms_error(0, "mms_queue", NULL, "Incorrectly formatted line %s in queue file %s!",
			  line, xqf);
	       else {
		    char *value = ptmp + 1;
		    char hname[512];
		    int xlen = (ptmp - res < sizeof hname) ? ptmp - res : -1 + sizeof hname;
		    strncpy(hname, res, xlen);
		    hname[xlen] = 0; /* terminate it. */
		    http_header_add(e->hdrs, hname, value);
	       }
	       break;
	  case '.':
	       okfile = 1;
	       break;
	  default:
	       mms_error(0, "mms_queue", NULL, "Unknown QF header %c in file %s!", ch, xqf);
	       break;
	  }
	  octstr_destroy(s);
	  if (okfile) 
	       break; /* We are done. */
     }
     parse_context_destroy(p);
     octstr_destroy(qdata);

     /* We should properly validate the queue file here. */
     if (!okfile) {
	  free_envelope(e,0);
	  e = NULL;
	  mms_error(0, "mms_queue", NULL, "Corrupt queue control file: %s",  xqf);
     }
     return e;     
}


/* Updates envelope to queue file:
 * - opens temp file
 * - writes output to temp file, if not new else writes directly.
 * - renames temp file to queue file (if not new)
 * This function doesn't check that this envelope is useless (i.e. no recipients)
 * - If function returns -1, caller should check errno for error.
 */
static int writeenvelope(MmsEnvelope *e, int newenv)
{
     Octstr *tfname = NULL;
     char *s;
     char buf[512];
     int fd;
     int i, n;
     int res = 0;
     struct qfile_t *qfs = e ? e->qfs_data : NULL;
     
     gw_assert(e);
     
     if (newenv)
	  fd = qfs->fd;
     else {
	  tfname = octstr_format( 
	       "%s/%s%c%s.%d", qfs->dir, qfs->subdir,
	       MTF, qfs->name + 1, random());
	  fd = open(octstr_get_cstr(tfname), 
		    O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	  if (fd < 0 ) { 
	       mms_error(0, "mms_queue", NULL, "Failed to open temp file %s: error = %s\n", 
		     octstr_get_cstr(tfname), strerror(errno));
	       res = -1;	  
	       goto done;
	  } else if (mm_lockfile(fd, octstr_get_cstr(tfname), 0) != 0) { /* Lock it. */
	       mms_error(0, "mms_queue", NULL, "Failed lock  temp file %s: error = %s\n", 
		     octstr_get_cstr(tfname), strerror(errno));
	       res = -1;	  
	       goto done;
	  }
     }
	  
     /* Write out. */

     s = (char *)mms_message_type_to_cstr(e->msgtype);
     if (!s) {
	  mms_error(0,  "mms_queue", NULL, "Write: Unknown MMS message type %d! Skipped\n", e->msgtype);
	  s = "";
     }
     _putline(fd, "T", s);
     
     if (e->msgId) 
	  _putline(fd, "I", octstr_get_cstr(e->msgId));

     if (e->mclass)
	  _putline(fd, "c", octstr_get_cstr(e->mclass));

     if (e->src_interface[0])
	  _putline(fd, "i", e->src_interface);
     
     if (e->from)
	  _putline(fd, "F", octstr_get_cstr(e->from));

     if (e->to)
	  n = gwlist_len(e->to);
     else
	  n = 0;

     for (i = 0; i < n; i++) {
	  MmsEnvelopeTo *to = gwlist_get(e->to, i);	  
	  _putline(fd, (to->process) ? "R" : "z", octstr_get_cstr(to->rcpt));
     }

     /* Output headers if any. */
     n = (e->hdrs) ? gwlist_len(e->hdrs) : 0;
     for (i = 0; i < n; i++) {
	  Octstr *h = NULL, *v = NULL;

	  http_header_get(e->hdrs, i, &h, &v);
	  if (h && v) {
	       Octstr *x = octstr_format("%s:%s", octstr_get_cstr(h), 
					 octstr_get_cstr(v));
	       _putline(fd, "H", octstr_get_cstr(x));
	       octstr_destroy(x);	       
	  }
	  if (h) octstr_destroy(h);
	  if (v) octstr_destroy(v);

     }

     sprintf(buf, "%ld", e->created);
     _putline(fd, "C", buf);

     if (e->lasttry) {
	  sprintf(buf, "%ld", e->lasttry);
	  _putline(fd, "L", buf);
     }

     if (e->sendt) {
	  sprintf(buf, "%ld", e->sendt);
	  _putline(fd, "D", buf);
     }

     if (e->expiryt) {
	  sprintf(buf, "%ld", e->expiryt);
	  _putline(fd, "X", buf);
     }

     if (e->attempts) {
	  sprintf(buf, "%ld", e->attempts);
	  _putline(fd, "N", buf);
     }

     if (e->lastaccess) {
	  sprintf(buf, "%ld", e->lastaccess);
	  _putline(fd, "f", buf);
     }

     sprintf(buf, "%ld", e->msize);
     _putline(fd, "S", buf);


     if (e->fromproxy) 
	  _putline(fd, "P", octstr_get_cstr(e->fromproxy));


     if (e->mdata) 
	  _putline(fd, "M", octstr_get_cstr(e->mdata));

     if (e->subject)
	  _putline(fd, "s", octstr_get_cstr(e->subject));
     

     if (e->viaproxy) 
	  _putline(fd, "p", octstr_get_cstr(e->viaproxy));

     if (e->token) 
	  _putline(fd, "t", octstr_get_cstr(e->token));
     

      if (e->vaspid) 
	  _putline(fd, "V", octstr_get_cstr(e->vaspid));
     
      if (e->vasid) 
	  _putline(fd, "v", octstr_get_cstr(e->vasid));
     
      if (e->url1) 
	  _putline(fd, "U", octstr_get_cstr(e->url1));

      if (e->url2) 
	  _putline(fd, "u", octstr_get_cstr(e->url2));

     if (e->dlr) 
	  _putline(fd, "r", "Yes");

     if (e->bill.billed) {
	  sprintf(buf, "%.3f", e->bill.amt);
	  _putline(fd,"b", buf);
     }

     _putline(fd, "", ".");

     fsync(fd); /* Sync data. */
     
     if (!newenv) { /* An update */
	  Octstr *qfname;
	 
	  qfname = octstr_format("%s/%s%s", qfs->dir, 
				 qfs->subdir,
				 qfs->name);	  
	  if (rename(octstr_get_cstr(tfname), octstr_get_cstr(qfname)) < 0) {
	       mms_error(0,  "mms_queue", NULL, "Failed to rename %s to %s: error = %s\n", 
		     octstr_get_cstr(qfname), octstr_get_cstr(tfname), strerror(errno));

	       close(fd); /* Close new one, keep old one. */
	       res = -1;	        
	  } else { /* On success, new descriptor replaces old one and we close old one. */
	       unlock_and_close(qfs->fd);
	       qfs->fd = fd;
	  }
	  octstr_destroy(qfname);
     }

 done:
     octstr_destroy(tfname);
     return res;
}


#define MAXTRIES 10

/* Makes a qf file in the queue directory. 
 * Makes several attempts then fails (returns -1) if it can't, fd otherwise
 * puts queue file name in qf (without directory name).
 * It is up to the caller to lock the file descriptor if needed.
 */
static int mkqf(char qf[QFNAMEMAX], char subdir[64], char *mms_queuedir)
{
     Octstr *xqf = NULL;
     char *ctmp;
     int i = 0, fd = -1;
     static int ect;
     
     if (!mms_queuedir)
	  gw_panic(0, "Queue directory passed as null!");
     
     /* First we decide the directory into which it goes... */
     if ((i = random() % 3) == 0) /* toplevel. */
	  subdir[0] = 0; 
     else if (i == 1)  /* one in */
	  sprintf(subdir, "%c/", _TT[random() % _TTSIZE]);
     else { /* two in. */
	  char csubdir[QFNAMEMAX];
	  sprintf(subdir, "%c/%c%c/", 
		  _TT[random() % _TTSIZE],
		  _TT[random() % _TTSIZE],
		  _TT[random() % _TTSIZE]);
	  
	  sprintf(csubdir, "%s/%s", mms_queuedir, subdir);
     	  if (mkdir(csubdir, 
		    S_IRWXU|S_IRWXG) < 0 && 
	      errno != EEXIST) {
	       mms_error(0,  "mms_queue", NULL,  "Failed to create dir %s - %s!",
		     csubdir, strerror(errno));
	       return -1;
	  }
     }

     do {
	  Octstr *tmp;
	  xqf = octstr_format("%cf%ld.%d.x%d.%ld", 
			       MQF, 
			      (long)time(NULL) % 10000, 
			      (++ect % 10000), getpid()%1000, random() % 100);
	  tmp = octstr_format("%.64s/%s%S", mms_queuedir, subdir, xqf);
	  
	  ctmp = octstr_get_cstr(tmp);
	  fd = open(ctmp, O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	  if (fd >= 0 && 
	      mm_lockfile(fd,ctmp,1) != 0) {
	       unlink(ctmp);
	       unlock_and_close(fd);
	       fd = -1;
	  }
	  octstr_destroy(tmp);
	  if (fd >= 0) 
	       break;
	  

	  octstr_destroy(xqf);
	  xqf = NULL;	  
     } while (i++ < MAXTRIES);
	       
     if (fd >= 0) 	  
	  strncpy(qf, octstr_get_cstr(xqf), QFNAMEMAX);    
     
     if (xqf) octstr_destroy(xqf);

     return fd;
}

static int writemmsdata(Octstr *ms, char *df, char subdir[], char *mms_queuedir)
{
     Octstr *dfname;
     int fd, n, res = 0;

     
     dfname = octstr_format("%s/%s%s", mms_queuedir, subdir, df);

     fd = open(octstr_get_cstr(dfname), 
	       O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);	  
     if (fd < 0) {
	  mms_error(0,  "mms_queue", NULL, "Failed to open data file %s: error = %s\n",
		octstr_get_cstr(dfname), strerror(errno));
	  res = -1;	
	  goto done;
     }

     n = octstr_write_to_socket(fd, ms);
     close(fd);
	  
     if (n != 0) {
	  mms_error(0,  "mms_queue", NULL, "Failed to write data file %s: error = %s\n",
		octstr_get_cstr(dfname), strerror(errno));
	  unlink(octstr_get_cstr(dfname));
	  res = -1;
     }

 done:
     octstr_destroy(dfname);
     return res;
}


Octstr *copy_and_clean_address(Octstr *addr)
{
     Octstr *s;
     int k, i;

     if (addr == NULL) return NULL;

     s = octstr_duplicate(addr);
     octstr_strip_blanks(s);
     /* Only clean up email addresses for now. */     
     if ((k = octstr_search_char(s, '@',0)) < 0)
	  goto done;

     /* Find '<' if any, then find last one and remove everything else. */
     
     i = octstr_search_char(s, '<',0);
     if (i >= 0) {
          int j;
	  
	  octstr_delete(s, 0, i+1); /* strip first part off. */
	  
	  j = octstr_search_char(s, '>',0);
          if (j >= 0) 
	       octstr_delete(s, j, octstr_len(s));	  
     } else {
	  /* remove everything after the domain. */
	  int n = octstr_len(s);
	  char *p = octstr_get_cstr(s);

	  
	  for (i = k+1; i < n; i++)
	       if (isspace(p[i])) { /* there can't be space in domain, so this marks end of address. */
		    octstr_delete(s, i, n);
		    break;
	       }
     }
     
done:
     if (octstr_len(s) == 0) {
	  octstr_destroy(s);
	  s = NULL;
     }
     return s;
}

static Octstr *mms_queue_add(Octstr *from, List *to, 
			     Octstr *subject,
			     Octstr *fromproxy, Octstr *viaproxy, 
			     time_t senddate, time_t expirydate, MmsMsg *m, Octstr *token, 
			     Octstr *vaspid, Octstr *vasid,
			     Octstr *url1, Octstr *url2,
			     List *hdrs,
			     int dlr,
			     char *directory, 
			     char *src_interface, 
			     Octstr *mmscname)
{
     char qf[QFNAMEMAX], subdir[64];
     int fd;
     MmsEnvelope *e;
     Octstr *ms = NULL, *res = NULL;
     struct qfile_t *qfs = NULL;
     
     fd = mkqf(qf, subdir, directory);     
     if (fd < 0) { 
	  mms_error(0,  "mms_queue", NULL, "%s: Failed err=%s\n", directory, strerror(errno));
	  return NULL;
     }

     res = xmake_qf(qf, subdir);      

     e = mms_queue_create_envelope(from, to, subject, fromproxy,viaproxy,
				   senddate,expirydate,token,vaspid,vasid,
				   url1,url2,hdrs,dlr,mmscname,m,
				   octstr_get_cstr(res), src_interface,
				   sizeof(struct qfile_t), &ms);
     qfs = e->qfs_data;
     strncpy(qfs->name, qf, sizeof qfs->name);
     strncpy(qfs->subdir, subdir, sizeof qfs->subdir);
     strncpy(qfs->dir, directory, sizeof qfs->dir);

     qfs->fd = fd;
     
     /* Write queue data. */
     if (writeenvelope(e, 1) < 0) {
	  octstr_destroy(res);
	  res = NULL;
	  goto done;
     }

     /* Write actual data before relinquishing lock on queue file. */

     qf[0]= MDF;
     if (writemmsdata(ms, qf, subdir, directory) < 0) {
	  octstr_destroy(res);
	  res = NULL;
	  goto done;
     }

 done:     
     free_envelope(e, 0);
     octstr_destroy(ms);
     return res;
}

static int free_envelope(MmsEnvelope *e, int removefromqueue)
{
     struct qfile_t *qfs;
     if (e == NULL)
	  return 0;
     qfs = e->qfs_data;
     if (removefromqueue) {
	  char fname[2*QFNAMEMAX];

	  
	  snprintf(fname, -1 + sizeof fname, "%s/%s%s", qfs->dir, qfs->subdir,  qfs->name); 
	  unlink(fname);
	  qfs->name[0] = MDF;	  
	  snprintf(fname, -1 + sizeof fname, "%s/%s%s", qfs->dir, qfs->subdir, qfs->name); 
	  unlink(fname);	  
     }
     unlock_and_close(qfs->fd); /* close and unlock now that we have deleted it. */

     mms_queue_free_envelope(e);

     return 0;     
}

void mms_queue_free_envelope(MmsEnvelope *e)
{
     MmsEnvelopeTo *x;
     
     if (e == NULL)  return;
     octstr_destroy(e->msgId);
     
     while ((x = gwlist_extract_first(e->to)) != NULL) {
	  octstr_destroy(x->rcpt);
	  gw_free(x);	  
     }
     gwlist_destroy(e->to, NULL);
     
     octstr_destroy(e->from);
     octstr_destroy(e->fromproxy);     
     octstr_destroy(e->mdata);
     octstr_destroy(e->viaproxy);
     octstr_destroy(e->token);
     octstr_destroy(e->subject);
     octstr_destroy(e->vaspid);
     octstr_destroy(e->vasid);
     octstr_destroy(e->url1);
     octstr_destroy(e->url2);
     octstr_destroy(e->mclass);

     http_destroy_headers(e->hdrs);

     gw_free(e);     

}

MmsEnvelope *mms_queue_create_envelope(Octstr *from, List *to, 
				       Octstr *subject,
				       Octstr *fromproxy, Octstr *viaproxy, 
				       time_t senddate, time_t expirydate,
				       Octstr *token, 
				       Octstr *vaspid, Octstr *vasid,
				       Octstr *url1, Octstr *url2,
				       List *hdrs,
				       int dlr,
				       Octstr *mmscname,
				       MmsMsg *m, 
				       char *xqfname,
				       char *src_interface,
				       int extra_space, 
				       Octstr **binary_mms)
{
     MmsEnvelope *e;
     Octstr *msgid = NULL, *ms = NULL, *r, *xfrom, *mclass = NULL;
     int mtype = -1, i, n;
     
     if (m) {
	  mtype = mms_messagetype(m);     
	  /* Get MsgID,  Fixup if not there and needed. */
	  if ((msgid = mms_get_header_value(m, octstr_imm("Message-ID")))  == NULL && 
	      xqfname) {     
	       msgid = mms_make_msgid(xqfname, mmscname);
	       if (mtype == MMS_MSGTYPE_SEND_REQ)
		    mms_replace_header_value(m, "Message-ID", octstr_get_cstr(msgid));
	  }    
	  ms = mms_tobinary(m);	  

	  mclass = mms_get_header_value(m, octstr_imm("X-Mms-Message-Class"));
     }

     xfrom = copy_and_clean_address(from);
     e = gw_malloc(extra_space + sizeof *e);      /* Make envelope, clear it. */
     memset(e, 0, sizeof *e);
     
     e->qfs_data = (void *)(e+1); /* pointer to data object for module. */

     e->msgtype = mtype;
     e->from = xfrom;
     e->created = time(NULL);
     e->sendt = senddate;
     e->expiryt = expirydate ? expirydate : time(NULL) + DEFAULT_EXPIRE;
     e->lasttry = 0;
     e->attempts = 0;
     e->lastaccess = 0;
     e->fromproxy = fromproxy ? octstr_duplicate(fromproxy) : NULL;
     e->viaproxy = viaproxy ? octstr_duplicate(viaproxy) : NULL;
     e->subject = subject ? octstr_duplicate(subject) : NULL;
     e->to = gwlist_create();
     e->msize = ms ? octstr_len(ms) : 0;
     e->msgId = msgid;
     e->token = token ? octstr_duplicate(token) : NULL;
     e->vaspid = vaspid ? octstr_duplicate(vaspid) : NULL;
     e->vasid = vasid ? octstr_duplicate(vasid) : NULL;
     e->url1 = url1 ? octstr_duplicate(url1) : NULL;
     e->url2 = url2 ? octstr_duplicate(url2) : NULL;
     e->hdrs = hdrs ? http_header_duplicate(hdrs) : http_create_empty_headers();     
     e->mclass = mclass;


     e->dlr = dlr;
     
     strncpy(e->src_interface, src_interface ? src_interface : "", sizeof e->src_interface);
     
     if (xqfname)
	  strncpy(e->xqfname, xqfname, sizeof e->xqfname);
     
     for (i = 0, n = to ? gwlist_len(to) : 0; i<n; i++) 
	  if ((r = gwlist_get(to, i)) != NULL && 
	      (r = copy_and_clean_address(r)) != NULL) {
	       MmsEnvelopeTo *t = gw_malloc(sizeof *t);
	       
	       t->rcpt = r;
	       t->process = 1;	       
	       gwlist_append(e->to, t);
	  }
     if (binary_mms)
	  *binary_mms = ms;
     else 
	  octstr_destroy(ms);

     return e;
}

static int mms_queue_free_env(MmsEnvelope *e)
{     
     return free_envelope(e, 0);
}

static int mms_queue_update(MmsEnvelope *e)
{
     int i, n = (e && e->to) ? gwlist_len(e->to) : 0;
     int hasrcpt = 0;
     MmsEnvelopeTo *x;     

     if (!e) return -1;
     /* FIX: Don't allow expiry to be <= 0 */
     if (e->expiryt <= 0)
	  e->expiryt = time(NULL) + DEFAULT_EXPIRE;
     for (i = 0; i < n; i++)	  
	  if ((x = gwlist_get(e->to, i)) != NULL && 
	      x->process) {
	       hasrcpt = 1;
	       break;
	  }
     
     if (!hasrcpt) {
	  free_envelope(e,1);
	  return 1;
     } else 
	  return writeenvelope(e, 0);         
}

static int mms_queue_replacedata(MmsEnvelope *e, MmsMsg *m)
{
     Octstr *tfname;
     Octstr *ms;
     struct qfile_t *qfs;
     int ret = 0;
     
     if (!e) return -1;

     qfs = e->qfs_data;
     tfname = octstr_format(".%c%s.%ld.%d", MDF, qfs->name + 1, time(NULL), random());     
     ms = mms_tobinary(m);     
     if (writemmsdata(ms, octstr_get_cstr(tfname), qfs->subdir, qfs->dir) < 0) 
	  ret = -1;
     else {
	  Octstr *fname = octstr_format("%s/%s%c%s", qfs->dir, qfs->subdir, MDF, qfs->name + 1);
	  Octstr *tmpf = octstr_format("%s/%s%S", qfs->dir, qfs->subdir, tfname);
	  if (rename(octstr_get_cstr(tmpf), octstr_get_cstr(fname)) < 0) {
	       mms_error(0,  "mms_queue", NULL, "mms_replacedata: Failed to write data file %s: error = %s\n",
		     octstr_get_cstr(tmpf), strerror(errno));	       
	       ret = -1;
	       unlink(octstr_get_cstr(tmpf)); /* remove it. */
	  } 	     
	  octstr_destroy(fname);
	  octstr_destroy(tmpf);
     }
     octstr_destroy(ms);

     octstr_destroy(tfname);
     return ret;     
}

static MmsMsg *mms_queue_getdata(MmsEnvelope *e)
{
     Octstr *fname;
     Octstr *ms;
     MmsMsg *m;
     struct qfile_t *qfs;
     
     if (!e) return NULL;
     qfs = e->qfs_data;
     
     fname = octstr_format("%s/%s%c%s", qfs->dir, qfs->subdir, MDF, qfs->name + 1);
     ms = octstr_read_file(octstr_get_cstr(fname));
     if (!ms) {
	  mms_error(0,  "mms_queue", NULL, "mms_queue_getdata: Failed to load data file for queue entry %s in %s",
		qfs->name, qfs->dir);
	  octstr_destroy(fname);
	  return NULL;
     }
     m = mms_frombinary(ms, octstr_imm(""));
     if (!m) {
	  mms_error(0,  "mms_queue", NULL, "mms_queue_getdata: Failed to decode data file for queue entry %s in %s",
		qfs->name, qfs->dir);
	  octstr_destroy(fname);
	  return NULL;
     }
     octstr_destroy(ms);
     octstr_destroy(fname);

     return m;     
}


struct Qthread_t {
     List *l;
     int (*deliver)(MmsEnvelope *e);
     long thid;
};

static void tdeliver(struct Qthread_t *qt)
{
     MmsEnvelope *e;

     while ((e = gwlist_consume(qt->l)) != NULL) {
	  int res;
	  res = qt->deliver(e); /* If it is on the queue, it has to be delivered. */

	  if (res != 1) /* Then delete as it wasn't deleted. */
	       free_envelope(e, 0);	       	  	  
     }     
     /* Consume failed, time to go away. */
     if (qt->l)
	  gwlist_destroy(qt->l, NULL);
     qt->l = NULL; /* Signal that we are gone. */
}


/* runs over a single directory, running queue items. return -1 if failed to run some item.
 * each directory found is pushed onto stack for future processing. 
 * dir must have trailing slash
 * return value of -2 means quit. 
 */
static int run_dir(char *topdir, char *dir, struct Qthread_t *tlist, int num_threads, int *i, List *stack)
{
     DIR *dirp;
     struct dirent *dp;

     Octstr *tdir = octstr_format("%s/%s", topdir, dir);
     char *xdir = octstr_get_cstr(tdir);
     int ret = 0;

     dirp = opendir(xdir);
	  
     if (!dirp) {
	  mms_error(0,  "mms_queue", NULL, "mms_queue_run: Failed to read queue directory %s, error=%s", 
		xdir, strerror(errno));
	  ret = -1;
	  goto done;
     }
	  
     while ((dp = readdir(dirp)) != NULL) {
	  struct stat st;
	  Octstr *xfname = octstr_format("%s%s", xdir, dp->d_name);
	  int sres = stat(octstr_get_cstr(xfname), &st);
	  time_t tnow = time(NULL);	  

	  octstr_destroy(xfname);

	  if (sres == 0 && S_ISREG(st.st_mode) && 
	      dp->d_name[0] == MQF && 
	      dp->d_name[1] == 'f') {
	       Octstr *xqf = xmake_qf(dp->d_name, dir);
	       MmsEnvelope *e = mms_queue_readenvelope(octstr_get_cstr(xqf),topdir, 0);
	       
	       octstr_destroy(xqf);
	       
	       if (!e) 
		    continue;
	       
	       if (e->sendt <= tnow) {
		    int queued = 0;
		    int j = *i; /* This is the next thread to use. Checking for cycles. */
		    do {
			 if (tlist[*i].l) {
			      debug("queuerun", 0, "Queued to thread %d for %s%s, sendt=%d, tnow=%d", 
				    *i, xdir, dp->d_name, (int)e->sendt, (int)tnow);
			      gwlist_produce(tlist[*i].l, e);
			      queued = 1;
			 }
			 *i = (*i+1)%num_threads;
		    }  while (!queued && *i != j);
		    
		    if (!queued) { /* A problem. There are no sender threads! */
			 free_envelope(e, 0);	       			
			 mms_error(0,  "mms_queue", NULL, "mms_queue_run: No active sender queues for directory %s. Quiting.",
			       xdir);
			 ret = -2; 
			 break;			      
		    }
	       } else
		    free_envelope(e,0); /* Let go of it. */
	       
	  } else if (sres == 0 && S_ISDIR(st.st_mode) &&
		     strcmp(dp->d_name, ".") != 0 &&
		     strcmp(dp->d_name, "..") != 0) {
	       Octstr *newdir = octstr_format("%s%s/", dir, dp->d_name); 
	       gwlist_append(stack, newdir); /* push it... */
	  }			  
     }
     if (dirp) closedir(dirp);	       
 done:

     octstr_destroy(tdir);
     return ret;
}
     
static void mms_queue_run(char *dir, 
			  int (*deliver)(MmsEnvelope *), 
			  double sleepsecs, int num_threads, volatile sig_atomic_t *rstop)
{
     struct Qthread_t *tlist;
     int i, qstop = 0;
     List *stack = gwlist_create();
     
     gw_assert(num_threads>0);
     
     tlist = gw_malloc(num_threads*sizeof tlist[0]);
     
     for (i = 0; i<num_threads; i++) { /* Create threads for sending. */
	  tlist[i].l = gwlist_create();
	  gwlist_add_producer(tlist[i].l);
	  tlist[i].deliver = deliver;	
	  tlist[i].thid = gwthread_create((gwthread_func_t *)tdeliver, &tlist[i]);
     }
     


     i = 0;  /* For stepping through above array. */
     do { 
	  Octstr *xdir = NULL;	  
	  gwlist_append(stack, octstr_create("")); /* Put initial dir on there. */

	  while (!*rstop && 
		 (xdir = gwlist_extract_first(stack)) != NULL) {
	       int ret = run_dir(dir, octstr_get_cstr(xdir), tlist, num_threads, &i, stack);
	       octstr_destroy(xdir);
	       xdir = NULL;
	       if (ret < 0) {		    
		    if (ret <= -2)
			 qstop = 1;
		    goto qloop;
	       }
	  }

	  octstr_destroy(xdir);
	  if (*rstop) 
	       break;
     qloop:
	  gwthread_sleep(sleepsecs);
     } while (!qstop);

     mms_info(0, "mms_queue", NULL, "Queue runner [%s] goes down...", dir);
     /* We are out of the queue, time to go away. */
     for (i = 0; i<num_threads; i++)
	  if (tlist[i].l)
	       gwlist_remove_producer(tlist[i].l);

     for (i = 0; i<num_threads; i++) /* Wait for them all to terminate. */
	  if (tlist[i].thid >= 0)
	       gwthread_join(tlist[i].thid); 
  

     for (i = 0; i<num_threads; i++)
	  if (tlist[i].l)
	       gwlist_destroy(tlist[i].l,NULL); /* Final destroy if needed. */
     gw_free(tlist);
     

     gwlist_destroy(stack, (gwlist_item_destructor_t *)octstr_destroy);

     return;
}

static int mms_cleanup_queue_module(void)
{
     return 0;
}

/* export functions... */
MmsQueueHandlerFuncs default_qfuncs = {
     mms_init_queue_module,
     mms_init_queue_dir,
     mms_cleanup_queue_module,
     mms_queue_add,   
     mms_queue_update,
     mms_queue_getdata,
     mms_queue_replacedata,
     mms_queue_readenvelope,
     mms_queue_run,
     mms_queue_free_env
};


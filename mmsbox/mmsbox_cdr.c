/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni MMSBOX base CDR handler module (writes to a CVS file)
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include <stdio.h>
#include <stdlib.h>

#include "mmsbox_cdr.h"


static FILE *cdr_file;
static List *req_list;
static long th_id;


#define CBUFSIZE 256
typedef struct MmsBoxCdrStruct {
     time_t sdate;
     char from[CBUFSIZE];
     char to[CBUFSIZE];
#if 0
     char src_ip[CBUFSIZE/4];
     char dest_ip[CBUFSIZE/4];
#endif
     char msgid[CBUFSIZE];
     char mmsc_id[CBUFSIZE];

     char src_int[CBUFSIZE/4];
     char dst_int[CBUFSIZE/4];

     unsigned long msg_size;     

     char msgtype[CBUFSIZE/8]; 
     char prio[CBUFSIZE/8]; 
     char mclass[CBUFSIZE/8]; 
     char status[CBUFSIZE/8]; 
     unsigned char dlr;
     unsigned char rr;
} MmsBoxCdrStruct;


static void cdr_logger_func(void)
{
     
     MmsBoxCdrStruct *cdr;

     while ((cdr = gwlist_consume(req_list)) != NULL) {
	  char buf[CBUFSIZE];
	  struct tm tm;
	  
	  localtime_r(&cdr->sdate, &tm);
	  gw_strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
	  fprintf(cdr_file, "%s", buf);
	  
	  /* Print rest of fields one by one. */
#define PRINTF_STR_FIELD(fld) fprintf(cdr_file, "\t%.*s", (int)(sizeof cdr->fld), cdr->fld)

	  PRINTF_STR_FIELD(src_int);
	  PRINTF_STR_FIELD(dst_int);
	  PRINTF_STR_FIELD(from);
	  PRINTF_STR_FIELD(to);
	  PRINTF_STR_FIELD(msgid);
	  PRINTF_STR_FIELD(mmsc_id);
	  PRINTF_STR_FIELD(msgtype);
	  PRINTF_STR_FIELD(prio);
	  PRINTF_STR_FIELD(mclass);
	  PRINTF_STR_FIELD(status);
	  
	  fprintf(cdr_file, "\t%ld\t%s\t%s\n", 
		  (long)cdr->msg_size, 
		  cdr->dlr ? "Yes" : "No",
		  cdr->rr ? "Yes" : "No");
	  
	  fflush(cdr_file);

	  gw_free(cdr);
     } 
     
}

static int cdr_module_init(char *settings)
{
     cdr_file = fopen(settings, "a"); /* Settings is interpreted as a file name. */
     if (cdr_file) {
	  req_list = gwlist_create();

	  gwlist_add_producer(req_list);

	  th_id = gwthread_create((void *)cdr_logger_func, NULL);
	  return 0;
     } else 
	  return -1;
}


static int module_fini(void)
{
     gw_assert(req_list);
     gw_assert(cdr_file);
     
     gwlist_remove_producer(req_list);

     if (th_id >= 0)
	  gwthread_join(th_id);
     gwlist_destroy(req_list, NULL); /* there shouldn't be any requests waiting */

     if (cdr_file) 
	  fclose(cdr_file); 

     return 0;
}

/* utility function. */
static void fill_cdr_struct(MmsBoxCdrStruct *cdr, 
			    time_t sdate, char *from, char *to, char *msgid, 
			    char *mmsc_id, char *src_int, char *dst_int, 
#if 0
			    char *src_ip, char *dst_ip, 
#endif					
			    unsigned long msg_size,
			    char *msgtype, char *prio, char *mclass,
			    char *status,
			    int dlr, int rr)
{     
     memset(cdr, 0, sizeof cdr[0]);
     
     cdr->sdate = sdate;

#define COPY_CDR_FIELD(fld) if (fld)			\
	  strncpy(cdr->fld, fld, sizeof cdr->fld)
     
     COPY_CDR_FIELD(from);
     COPY_CDR_FIELD(to);
     COPY_CDR_FIELD(msgid);
     COPY_CDR_FIELD(mmsc_id);
     COPY_CDR_FIELD(src_int);
     COPY_CDR_FIELD(dst_int);
     COPY_CDR_FIELD(msgtype);
     COPY_CDR_FIELD(prio);
     COPY_CDR_FIELD(mclass);
     COPY_CDR_FIELD(status);
     
     cdr->dlr = dlr;
     cdr->rr = rr;
     cdr->msg_size = msg_size;     
}

static int cdr_module_logcdr(time_t sdate, char *from, char *to, char *msgid, 
			     char *mmsc_id, char *src_int, char *dst_int, 
#if 0
			     char *src_ip, char *dst_ip, 
#endif					
			     unsigned long msg_size,
			     char *msgtype, char *prio, char *mclass,
			     char *status,
			     int dlr, int rr)
{
     MmsBoxCdrStruct *xcdr = gw_malloc(sizeof *xcdr);
     
     gw_assert(req_list);
     
     
     fill_cdr_struct(xcdr, sdate, from, to, msgid, mmsc_id, src_int, dst_int,
#if 0
		     src_ip, dst_ip, 
#endif
		     msg_size, msgtype, prio, mclass, status, dlr, rr);

     gwlist_produce(req_list, xcdr);
     
     return 0;

}


MmsBoxCdrFuncStruct mmsbox_cdrfuncs = {
     cdr_module_init,
     cdr_module_logcdr,
     module_fini
};

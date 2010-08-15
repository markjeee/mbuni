/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni sample billing handler module
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

#include "mms_billing.h"

/* This module provides a basic biller and CDR implementation. Totally no frills, but a basis
 * for implementing a 'real' module.
 * It does CDR but no billing (of course). 
 */


static void *mms_billingmodule_init(char *settings)
{
     return fopen(settings, "a"); /* Settings is interpreted as a file name. */
}


static int mms_billingmodule_fini(void *module_data)
{
     return module_data ? fclose(module_data) : -1;
}

static int mms_billmsg(Octstr *from, List *to, unsigned long msg_size, Octstr *vaspid, Octstr *msgid, void *module_data)
{
     return 0;
}

static int mms_logcdr(MmsCdrStruct *cdr)
{
     if (cdr && cdr->module_data) {
	  char buf[CBUFSIZE];
	  struct tm tm;
	  
	  localtime_r(&cdr->sdate, &tm);
	  gw_strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
	  fprintf(cdr->module_data, "%s\t%.128s\t%.128s\t%.128s\t%.256s\t%.256s\t%ld\n",
		  buf, 
		  cdr->src_interface[0] ? cdr->src_interface : "MM2", 
		  cdr->dst_interface[0] ? cdr->dst_interface : "MM2", 
		  cdr->from,  cdr->to, cdr->msgid, cdr->msg_size);
	  fflush(cdr->module_data);
     } else
	  return -1;
     
     return 0;
}

MmsCdrStruct *make_cdr_struct(void *module_data, time_t sdate, char *from, char *to, char *msgid, 
			      char *vaspid, char *src_int, char *dst_int, unsigned long msg_size)
{

     MmsCdrStruct  *cdr = gw_malloc(sizeof *cdr);
     
     cdr->module_data = module_data;
     cdr->sdate = sdate;
     strncpy(cdr->from, from ? from : "", sizeof cdr->from);
     strncpy(cdr->to, to ? to : "", sizeof cdr->to);
     strncpy(cdr->msgid, msgid ? msgid : "", sizeof cdr->msgid);
     strncpy(cdr->vaspid, vaspid ? vaspid : "", sizeof cdr->vaspid);
     strncpy(cdr->src_interface, src_int ? src_int : "MM2", sizeof cdr->src_interface);
     strncpy(cdr->dst_interface, dst_int ? dst_int : "MM2", sizeof cdr->dst_interface);     
     
     cdr->msg_size = msg_size;
     
     return cdr;
}

/* The function itself. */
MmsBillingFuncStruct mms_billfuncs = {
     mms_billingmodule_init,
     mms_logcdr,
     mms_billmsg,
     mms_billingmodule_fini
};

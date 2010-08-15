/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni billing integration  interface
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#ifndef __MMS_BILLING_INCLUDED__
#define __MMS_BILLING_INCLUDED__


#include <time.h>
#include "gwlib/gwlib.h"

/* Billing and CDR module. This file provides prototypes for all billing functions.
 * The idea is that for each site a DSO will be created that the mmsglobalsender loads and gets 
 * from functions to do billing. This .h file provides prototypes for these functions. 
 * It has an attendant function to provide basic biller and CDR functions
 */
#define CBUFSIZE 256
typedef struct MmsCdrStruct {
     void *module_data;
     time_t sdate;
     char from[CBUFSIZE];
     char to[CBUFSIZE];
     char msgid[CBUFSIZE];
     char vaspid[CBUFSIZE];
     char src_interface[CBUFSIZE];
     char dst_interface[CBUFSIZE];
     unsigned long msg_size;     
} MmsCdrStruct;


typedef struct MmsBillingFuncStruct {
/* This function is called once to initialise the billing module. Return a generic object,
 * which is passed with each CDR.
 */
      void *(*mms_billingmodule_init)(char *settings);
     
/* This function logs a cdr to wherever it is logging to. */
      int (*mms_logcdr)(MmsCdrStruct *cdr);
     
/* Bills a message. Returns >= 0 if billed ok, -1 if message should be rejected, 
 * -2 on internal (temporary) error.
 */
      int (*mms_billmsg)(Octstr *from, List *to, unsigned long msg_size, Octstr *vaspid, Octstr *msgid, void *module_data);
     
      int (*mms_billingmodule_fini)(void *module_data);
} MmsBillingFuncStruct;

extern MmsBillingFuncStruct mms_billfuncs; /* The module must expose this symbol. */

/* utility function. */
MmsCdrStruct *make_cdr_struct(void *module_data, time_t sdate, char *from, char *to, char *msgid, 
			      char *vaspid, char *src_int, char *dst_int, unsigned long msg_size);
#endif

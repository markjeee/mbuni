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

#ifndef __MMSBOX_CDR_INCLUDED__
#define __MMSBOX_CDR_INCLUDED__


#include <time.h>
#include "gwlib/gwlib.h"

/* MMSBOX  CDR module. This file provides prototypes for all CDR functions.
 * 
 */


typedef struct MmsBoxCdrFuncStruct {
/* This function is called once to initialise the  module. Return 0 on success */
     int (*init)(char *settings);
     
     /* This function logs a cdr to wherever it is logging to. */
     int (*logcdr)(time_t sdate, char *from, char *to, char *msgid, 
		   char *mmsc_id, char *src_int, char *dst_int, 
#if 0
		   char *src_ip, char *dst_ip, 
#endif					
		   unsigned long msg_size,
		   char *msgtype, char *prio, char *mclass,
		   char *status,
		   int dlr, int rr);
     
     int (*cleanup)(void);
} MmsBoxCdrFuncStruct;

extern MmsBoxCdrFuncStruct mmsbox_cdrfuncs; /* The module must expose a symbol 'cdr_funcs' */

#endif

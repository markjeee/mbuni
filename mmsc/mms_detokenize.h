/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni MSISDN mapper interface
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_DETOKENIZE_INCLUDED__
#define __MMS_DETOKENIZE_INCLUDED__

#include <time.h>
#include "gwlib/gwlib.h"

/* Detokenizer module. This file provides prototypes for all detokenizer functions.
 * The idea is that for each site a DSO will be created that can resolve a token into an
 * msisdn. This is useful if you're creating a multioperator setup or if your wap gateway
 * doesn't pass the MSISDN as a header and you want to secure yourself against MSISDN spoofing
 */

typedef struct MmsDetokenizerFuncStruct {
/* This function is called once to initialise the detokenizer module. Return 0 on succeful
 * initialization.
 */
     int (*mms_detokenizer_init)(char *settings);
     
/* Looks up the token and returns the msisdn as a new Octstr.
 * Return NULL on error, otherwise an Octstr
 */
     Octstr *(*mms_detokenize)(Octstr * token, Octstr *request_ip);
     
/* Given an msisdn, returns the token associated
 * Return NULL on error, otherwise an Octstr
 */
     Octstr *(*mms_gettoken)(Octstr *msisdn);
     
     int (*mms_detokenizer_fini)(void);
} MmsDetokenizerFuncStruct;

extern MmsDetokenizerFuncStruct mms_detokenizefuncs; /* The module must expose this symbol. */

#endif

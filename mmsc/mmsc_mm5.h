/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MM5 interface, query/save mapping from msisdn to uaprof string/url
 *  
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_MM5_INCLUDED__
#define __MMS_MM5_INCLUDED__

#include <time.h>
#include "gwlib/gwlib.h"

/* mm5 module. This file provides prototypes for all mm5 interface functions.
 * 
 */

typedef struct MmscMM5FuncStruct {
/* This function is called once to initialise the mm5 interface module. Return a generic object,
 * which is passed with each request.
 */
      int (*init)(char *settings);
     
  /* Save the UaProf URL and UserAgent string for given phone number. */
     void (*update)(char *phonenum, 
		       char *uaprof_url, char *ua_string);
     
     /* Query for the uaprof and ua string. Return them in the buffers provided. 
      * return 0 on success, -1 on error.
      */
     int (*query)(char *phonenum, char uaprof_url[], char *ua_string, 
		      int buflen);     

     void (*cleanup)(void);
} MmscMM5FuncStruct;

/* Any module must export a symbol of the above type, with name 'mm5_funcs' */
extern MmscMM5FuncStruct *shell_mm5; /* Handler when using shell script. */

#endif

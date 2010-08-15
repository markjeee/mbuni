/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Resolving MSISDNs to local/remote MMSCs - interface
 *  
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_RESOLVE_INCLUDED__
#define __MMS_RESOLVE_INCLUDED__

#include <time.h>
#include "gwlib/gwlib.h"

/* Resolver module. This file provides prototypes for all resolver functions.
 * The idea is that for each site a DSO will be created that the mmsglobalsender loads and gets
 * functions to resolve msisdn's to mmsc addresses. If the string returned is the same as our
 * hostname, the msisdn is considered local.
 */

typedef struct MmsResolverFuncStruct {
/* This function is called once to initialise the resolver module. Return a generic object,
 * which is passed with each resolution request..
 */
      void *(*mms_resolvermodule_init)(char *settings);
     
/* Looks up the msisdn and returns the hostname of the msisdn's mmsc. If returned mmsc matches
 * our hostname, the user is considered local.
 *
 * Return NULL on error, otherwise an Octstr
 */
     Octstr *(*mms_resolve)(Octstr *phonenum, char *src_int, char *src_id, 
			    void *module_data, void *settings, void *proxyrelays);

      int (*mms_resolvermodule_fini)(void *module_data);

} MmsResolverFuncStruct;

extern MmsResolverFuncStruct mms_resolvefuncs; /* The module must expose this symbol. */

#endif

/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Resolving MSISDNs to local/remote MMSBox MMSC - interface
 *  
 * Copyright (C) 2003 - 2007, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMSBOX_RESOLVE_INCLUDED__
#define __MMSBOX_RESOLVE_INCLUDED__

#include <time.h>
#include "gwlib/gwlib.h"

/* Resolver module. This file provides prototypes for all resolver functions.
 * the module is loaded once and _init is called once at load. 
 * _resolve is called for each incoming message to determine how to route it.
 */

typedef struct MmsBoxResolverFuncStruct {
/* This function is called once to initialise the resolver module. Return a generic object,
 * which is passed with each resolution request..
 */
      void *(*mmsbox_resolvermodule_init)(char *settings);
     
/* Looks up the sender and receiver msisdns and returns the ID of the MMC connection through which 
 * the received message should be sent. 
 * Note: This function may modify sender and/or receive to match prefered usage.
 * Return NULL or the empty string to send the message to a service (normal behavior)
 */
      Octstr *(*mmsbox_resolve)(Octstr *pfrom, Octstr *pto, char *from_mmsc, 
				void *module_data, void *settings);

      int (*mmsbox_resolvermodule_fini)(void *module_data);
} MmsBoxResolverFuncStruct;

extern MmsBoxResolverFuncStruct mmsbox_resolvefuncs; /* The module must expose this symbol. */

#endif

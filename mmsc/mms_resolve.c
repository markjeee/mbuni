/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Resolving MSISDNs to local/remote MMSCs
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
#include "mms_resolve.h"
#include "mmsc_cfg.h"

static void *mms_resolvermodule_init(char *settings)
{
     return NULL;
}

static int mms_resolvermodule_fini(void *module_data)
{
     return 0;
}

static Octstr *mms_resolve(Octstr * phonenum, char *src_int, char *src_id, 
			   void *module_data, void *settings_p, void *proxyrelays_p)
{
     /* Most custom implementations of this library will probably just ignore the two last arguments,
      * but this one needs them
      */
     
     MmscSettings *settings = (MmscSettings *) settings_p;
     List *proxyrelays = (List *) proxyrelays_p;
     int j, m;
     
     if (does_prefix_match(settings->local_prefix, phonenum)) {
	  return settings->hostname ? octstr_duplicate(settings->hostname) : NULL;
     } else if (proxyrelays && gwlist_len(proxyrelays) > 0)	/* Step through proxies. */
	  for (j = 0, m = gwlist_len(proxyrelays); j < m; j++) {
	       MmsProxyRelay *mp = gwlist_get(proxyrelays, j);
	       if (does_prefix_match(mp->allowed_prefix, phonenum) && 
		   !does_prefix_match(mp->denied_prefix, phonenum)) {
		    return octstr_duplicate(mp->host);
	       }
	  }
     
     return 0;
}

/* The function itself. */
MmsResolverFuncStruct mms_resolvefuncs = {
     mms_resolvermodule_init,
     mms_resolve,
     mms_resolvermodule_fini
};

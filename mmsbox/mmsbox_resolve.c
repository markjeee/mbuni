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
#include <stdio.h>
#include <stdlib.h>
#include "mmsbox_resolve.h"
#include "mmsbox_cfg.h"

static void *_resolvermodule_init(char *settings)
{
     return NULL;
}

static int _resolvermodule_fini(void *module_data)
{
     return 0;
}

static Octstr *_resolve(Octstr * pfrom, Octstr *pto, char *in_mmsc,
			void *module_data, void *settings_p)
{
  /* route normally to mms-service. */
     return NULL;
}

/* The function itself. */
MmsBoxResolverFuncStruct mmsbox_resolvefuncs = {
     _resolvermodule_init,
     _resolve,
     _resolvermodule_fini
};

/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Resolving MSISDNs to local/remote MMSCs - calling shell scripts
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
#include "mmsbox_resolve.h"
#include "mms_util.h"

static Octstr *script = NULL;

static void *_shell_resolvermodule_init(char *settings)
{
     script = octstr_imm(settings);
     return NULL;
}

static int _shell_resolvermodule_fini(void *module_data)
{
     octstr_destroy(script);
     script = NULL;
     return 0;
}

static Octstr *_shell_resolve(Octstr *pfrom, Octstr *pto, char *in_mmc, void *module_data, void *settings_p)
{
     Octstr *s;
     FILE *fp;
     char buf[4096];

     if (script == NULL || octstr_len(script) == 0)
	  return 0;
     
     s = octstr_format("%s '%s' '%s' '%s'", 
		       octstr_get_cstr(script), octstr_get_cstr(pfrom), octstr_get_cstr(pto),
		       in_mmc);

     mms_info(0, "mmsbox", NULL, "Preparing to call resolver as: %s", octstr_get_cstr(s));
     fp = popen(octstr_get_cstr(s), "r");
     octstr_destroy(s);

     fgets(buf, sizeof buf, fp);
     s = octstr_create(buf);
     octstr_strip_crlfs(s);

     pclose(fp);

     mms_info(0, "mmsbox", NULL, "Resolver returned: %s", octstr_get_cstr(s));     
     if (octstr_len(s) == 0) {
        octstr_destroy(s);
        return NULL;
     }

     return s;
}

/* The function struct itself. */
MmsBoxResolverFuncStruct mmsbox_resolvefuncs_shell = {
     _shell_resolvermodule_init,
     _shell_resolve,
     _shell_resolvermodule_fini
};

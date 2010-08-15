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
#include "mms_resolve.h"
#include "mms_util.h"

static Octstr *script = NULL;

static void *mms_resolvermodule_init(char *settings)
{
	 script = octstr_imm(settings);
     return NULL;
}

static int mms_resolvermodule_fini(void *module_data)
{
     return 0;
}

static Octstr *mms_resolve(Octstr *phonenum, char *src_int, char *src_id, 
			   void *module_data, void *settings_p, void *proxyrelays_p)
{
     Octstr *s;
     FILE *fp;
     char buf[4096];

     if (script == NULL || octstr_len(script) == 0)
	  return 0;
     
     s = octstr_format("%s '%s' '%s' '%s'",
		       octstr_get_cstr(script), octstr_get_cstr(phonenum) ,
		       src_int ? src_int : "", src_id ? src_id : "");
     fp = popen(octstr_get_cstr(s), "r");
     octstr_destroy(s);

     fgets(buf, sizeof buf, fp);
     s = octstr_create(buf);
     octstr_strip_crlfs(s);

     pclose(fp);
     
     if (octstr_len(s) == 0) {
        octstr_destroy(s);
        return NULL;
     }

     return s;
}

/* The function itself. */
MmsResolverFuncStruct mms_resolvefuncs_shell = {
     mms_resolvermodule_init,
     mms_resolve,
     mms_resolvermodule_fini
};

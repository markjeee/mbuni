/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni sample billing handler module
 * 
 * Copyright (C) 2003 - 2005, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <stdio.h>
#include <stdlib.h>
#ifndef DARWIN
#include <wait.h>
#endif
#include <sys/wait.h>


#include "mms_billing.h"

static Octstr *script = NULL;

static void *mms_billingmodule_init(char *settings)
{
	 script = octstr_create(settings);
     return NULL;
}


static int mms_billingmodule_fini(void *module_data)
{
     if (script) {
	  octstr_destroy(script);
	  script = NULL;	  
     }
     return 0;
}

static int mms_billmsg(Octstr *from, List *to, unsigned long msg_size, Octstr *vaspid, Octstr *msgid, void *module_data)
{
     int i;

     if (script == NULL || octstr_len(script) == 0)
	  return 0;
     for (i=0;i<gwlist_len(to);i++) {
	  Octstr *s;
	  s = octstr_format("%s '%s' '%s' '%S'", octstr_get_cstr(script), octstr_get_cstr(from), 
			    octstr_get_cstr(gwlist_get(to, i)), msgid); 
	  if (s) {
	       int ret = system(octstr_get_cstr(s));	  
	       octstr_destroy(s);
	       if (ret < 0)
		    return -2;
	       else if (WEXITSTATUS(ret) != 0)
		    return -1;
	  }
     }
     return 0;
}

static int mms_logcdr(MmsCdrStruct *cdr)
{
     return 0;
}

/* The function itself. */
MmsBillingFuncStruct mms_billfuncs_shell = {
     mms_billingmodule_init,
     mms_logcdr,
     mms_billmsg,
     mms_billingmodule_fini
};

/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni MSISDN mapper shell caller
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
#include "mms_detokenize.h"
#include "mms_util.h"

Octstr *script;

static int mms_detokenizer_init(char *settings)
{
     script = octstr_create(settings);
     mms_info(0, "mms_detokenizer", NULL, "Detokenizer script set to \"%s\"", settings);
     return 0;
}

static int mms_detokenizer_fini(void)
{
     if (script) {
	  octstr_destroy(script);
	  script = NULL;	  
     }
     
     return 0;
}

static Octstr *mms_detokenize(Octstr *token, Octstr *request_ip)
{
     Octstr *cmd = NULL, *msisdn = NULL;
     Octstr *tmp1, *tmp2;
     FILE *fp;
     char buf[4096];

     if (script == NULL || octstr_len(script) == 0)
	  return NULL;
     
     tmp1 = octstr_duplicate(token ? token : octstr_imm("x"));
     tmp2 = octstr_duplicate(request_ip ? request_ip : octstr_imm("x"));
     
     escape_shell_chars(tmp1);
     escape_shell_chars(tmp2);
     
     cmd = octstr_format("%s %s %s", 
			 octstr_get_cstr(script), 
			 octstr_get_cstr(tmp1),
			 octstr_get_cstr(tmp2));
     octstr_destroy(tmp1);
     octstr_destroy(tmp2);
     mms_info(0, "mms_detokenizer", NULL, "Calling \"%s\"", octstr_get_cstr(cmd));
     if ((fp = popen(octstr_get_cstr(cmd), "r"))) {
	  if (fgets(buf, sizeof buf, fp) != NULL) {
	       msisdn = octstr_create(buf);
	       octstr_strip_crlfs(msisdn);
	  }
	  pclose(fp);
     }
     mms_info(0, "mms_detokenizer", NULL, "%s \"%s\", returned msisdn = %s", 
	  fp ? "Called" : "Failed to call", 
	  octstr_get_cstr(cmd),
	  msisdn ? octstr_get_cstr(msisdn) : "null");
     octstr_destroy(cmd);
     return msisdn;
}

static Octstr *mms_gettoken(Octstr *msisdn)
{
     /* Dummy. */
     return octstr_create("y");
}

/* The function itself. */
MmsDetokenizerFuncStruct mms_detokenizefuncs_shell = {
     mms_detokenizer_init,
     mms_detokenize,
     mms_gettoken,
     mms_detokenizer_fini
};

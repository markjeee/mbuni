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
#include <stdio.h>
#include <stdlib.h>

#include "mmsc_mm5.h"
#include "mms_util.h"

static char cmd[1024] = "true";
static int mm5_shell_init(char *shell_cmd)
{     
     strncpy(cmd, shell_cmd, sizeof cmd);
     return 0;
}

static void mm5_shell_update(char *phonenum, char *uaprof_url, char *ua_string)
{
     Octstr *x;
     Octstr *y;
     Octstr *z;
     Octstr *s;
     
     if (phonenum == NULL) 
	  return;
     
     x = uaprof_url ? octstr_create(uaprof_url) : octstr_imm("");
     y = ua_string ? octstr_create(ua_string) : octstr_imm("");
     z = octstr_create(phonenum);
     
     escape_shell_chars(x);
     escape_shell_chars(y);
     escape_shell_chars(z);
     s = octstr_format("%s '%S' '%S' '%S'", cmd, z, x, y);

     system(octstr_get_cstr(s));

     octstr_destroy(s);
     octstr_destroy(x);
     octstr_destroy(y);
     octstr_destroy(z);          
}


static int mm5_shell_query(char *phonenum, char uaprof_url[], char *ua_string, 
			   int buflen)
{     
     FILE *fp;
     char buf[4096], *p;
     int ret;
     
     Octstr *z;
     Octstr *s;
     
     if (phonenum == NULL) 
	  return -1;
     
     z = octstr_create(phonenum);
     escape_shell_chars(z);

     s = octstr_format("%s '%S'", cmd, z);     
     if ((fp = popen(octstr_get_cstr(s), "r")) != NULL) {
	  
	  if (uaprof_url) {
	       uaprof_url[0] = 0;
	       p = fgets(buf, sizeof buf, fp);
	       if (p) 
		    strncpy(uaprof_url, p, buflen);
	  }
	  
	  if (ua_string) {
	       ua_string[0] = 0;
	       p = fgets(buf, sizeof buf, fp);
	       if (p) 
		    strncpy(ua_string, p, buflen);
	  }
	  pclose(fp);
	  ret = 0;
     } else 
	  ret = -1;
     octstr_destroy(s);
     octstr_destroy(z);
     
     return ret;
}


static void shell_cleanup(void)
{

}

MmscMM5FuncStruct _x = {
     mm5_shell_init,
     mm5_shell_update,
     mm5_shell_query,
     
     shell_cleanup
}, *shell_mm5 = &_x;

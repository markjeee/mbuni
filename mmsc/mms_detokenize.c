/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni MSISDN mapper sample
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
#include "mms_detokenize.h"
#include "mms_util.h"

static int mms_detokenizer_init(char *settings)
{
     return 0;
}

static int mms_detokenizer_fini(void)
{
     return 0;
}

static Octstr *mms_detokenize(Octstr * token, Octstr *request_ip)
{
     /* Return the MSISDN matching the token as a new Octstr */
     return octstr_create("+45xxxxxx");
}

static Octstr *mms_gettoken(Octstr *msisdn)
{
     /* Return the MSISDN matching the token as a new Octstr */
     return octstr_create("yy");
}

/* The function itself. */
MmsDetokenizerFuncStruct mms_detokenizefuncs = {
     mms_detokenizer_init,
     mms_detokenize,
     mms_gettoken,
     mms_detokenizer_fini
};

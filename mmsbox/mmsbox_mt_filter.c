/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Empty wrapper library
 * 
 * Copyright (C) 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is proprietary software, refer to licence holder for details
 */ 
#include <sys/file.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <dlfcn.h>
#include <strings.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mmsbox_mt_filter.h"

static int myinit(Octstr *mmc_url, Octstr *mmc_id, Octstr *startup_params)
{     
     return 0;
}


static int myfilter(MIMEEntity **msg, Octstr *loc_url, Octstr *mmc_id)
{
     
     return 0;
}

static void myclose(Octstr *mmc_id)
{
     return;
}

struct MmsBoxMTfilter mmsbox_mt_filter = {
     .name = "Empty Wrapper",
     .init = myinit,
     .filter = myfilter,
     .destroy = myclose
};

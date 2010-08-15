/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBOX MT MMS filter: Optional filter for MT messages
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMSMT_FILTER_INCLUDED__
#define __MMSMT_FILTER_INCLUDED__
#include "mms_util.h"

struct MmsBoxMTfilter {
     /* name of filter. */
     char *name; 
     /* Init: called once for each mmc to determine if to use filter on MT MMS via the particular MMC. Returns 1
      * on success, error otherwise
      */
     int (*init)(Octstr *mmc_url, Octstr *mmc_id, Octstr *startup_params);     

     /* filter: Filter/transform the message. Return 0 on success. May modify the message itself of course */
     int (*filter)(MIMEEntity **msg, Octstr *loc_url, Octstr *mmc_id);

     /* destroy this mmc's settings in filter. */
     void (*destroy)(Octstr *mmc_id);
};

/* Each module must export this symbol, a pointer to the structure.
 * WARNING: Ensure your module is thread-safe 
 */
extern struct MmsBoxMTfilter mmsbox_mt_filter;
#endif

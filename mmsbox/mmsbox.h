/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBOX CFG: MMC configuration and misc. functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMSBOX_INCLUDED__
#define __MMSBOX_INCLUDED__
#include "mmsbox_cfg.h"

extern volatile sig_atomic_t rstop;
void mms_dlr_url_put(Octstr *msgid, char *rtype, Octstr *mmc_gid, Octstr *dlr_url, Octstr *transid);
int mms_dlr_url_get(Octstr *msgid, char *rtype, Octstr *mmc_gid, Octstr **dlr_url, Octstr **transid);
void mms_dlr_url_remove(Octstr *msgid, char *rtype, Octstr *mmc_gid);

Octstr *mmsbox_get_report_info(MmsMsg *m, MmscGrp *mmsc, Octstr *out_mmc_id, 
			       char *report_type, 
			       Octstr *status, List *qhdr, Octstr *uaprof, 
			       time_t uaprof_tstamp, 
			       Octstr *msgid);
void mmsc_receive_func(MmscGrp *m);
void mmsbox_outgoing_queue_runner(volatile sig_atomic_t *rstop);

/* Just a convenience, should go away in future! */
#define mmsbox_url_fetch_content mms_url_fetch_content
#endif

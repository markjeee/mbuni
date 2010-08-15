/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBOX Custom MMSC types: MMC function definitions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMSBOX_MMSC_CFG_INCLUDED__
#define __MMSBOX_MMSC_CFG_INCLUDED__
#include "mms_util.h"
#include "mms_queue.h"
struct MmscGrp; /* so we compile. */
typedef struct MmsBoxMmscFuncs {
     /* start_conn: called once with the module settings, ID of the connection
      * and a pointer where to store module specific info. 
      * should return 0 on success, -1 on error. 
      */
     int (*start_conn)(struct MmscGrp *mmc,  MmsQueueHandlerFuncs *qfs, 
		       Octstr *unified_prefix, List *strip_prefixes, 
		       void **data);
     
     /* stop_conn: Called to stop the MMC connection. */
     int (*stop_conn)(void *data);

     /* send_msg: called to send a message. Should msg ID if any. On error, 
      * retry can be set to 1 for sending to be retried later. 
      * Should set err to the error message (if any).
      */
     Octstr *(*send_msg)(void *data, Octstr *from, Octstr *to, 
			 Octstr *transid, 
			 Octstr *linkedid, char *vasid, Octstr *service_code,
			 MmsMsg *m, List *hdrs, Octstr **err, int *retry);
     
} MmsBoxMmscFuncs;

extern MmsBoxMmscFuncs mmsc_funcs; /* lib should expose this structure. */
#endif

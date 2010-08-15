/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS Relay, implements message routing
 * 
 * Copyright (C) 2003 - 2005, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include "mmsc.h"


static long qthread = -1;

static volatile sig_atomic_t rstop = 0; /* Set to 1 to stop relay. */

int mmsrelay()
{

     if (!(settings->svc_list & (SvcMM1 | SvcRelay)))  {
	  mms_info(0, "mmsrelay", NULL, " " MM_NAME " MMSC Relay version %s, no services to be started.", MMSC_VERSION);
	  return 0;
     } else 
	  mms_info(0,  "mmsrelay", NULL, " " MM_NAME " MMSC Relay  version %s starting", MMSC_VERSION);
     
     /* Start global queue runner. */
     if (settings->svc_list & SvcRelay) {
	  mms_info(0, "mmsrelay", NULL, "Starting Global Queue Runner...");
	  qthread = gwthread_create((gwthread_func_t *)mbuni_global_queue_runner, &rstop);
     }

     if (settings->svc_list & SvcMM1) {
	  /* Start the local queue runner. */
	  mms_info(0,  "mmsrelay", NULL,"Starting Local Queue Runner...");
	  mbuni_mm1_queue_runner(&rstop);
     }
     
     if (qthread >= 0)
	  gwthread_join(qthread); /* Wait for it to die... */
     mms_info(0,  "mmsrelay", NULL, "MMSC Relay MM1 queue runner terminates...");     
     return 0;
     
}

int stop_mmsrelay(void)
{
     rstop = 1;
     mms_info(0,  "mmsrelay", NULL, "Mmsrelay: Queue runners shutdown, cleanup commenced...");
     return 0;
};


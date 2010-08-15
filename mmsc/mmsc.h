/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSC: Full MMSC startup
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MBUNI_MMSC_INCLUDED__
#define __MBUNI_MMSC_INCLUDED__
#include "mmsc_cfg.h"

int  mmsproxy(void);
void stop_mmsproxy(void);

int mmsrelay(void);
int stop_mmsrelay(void);


extern void mbuni_global_queue_runner(volatile sig_atomic_t *stopflag);
extern void mbuni_mm1_queue_runner(volatile sig_atomic_t *stopflag);
extern  MmscSettings *settings;
extern List *proxyrelays;


#endif

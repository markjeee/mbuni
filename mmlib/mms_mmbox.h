/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Mbuni MMBox implementation
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_MMBOX__INCLUDED__
#define __MMS_MMBOX__INCLUDED__
#include "mms_msg.h"
/* Initialise the root of the mmbox. Should be called once from load settings. */
int mmbox_root_init(char *mmbox_root);
Octstr *mms_mmbox_addmsg(char *mmbox_root, char *user, MmsMsg *msg, List *flag_cmds, Octstr *dfltstate);
int mms_mmbox_modmsg(char *mmbox_root, char *user, Octstr *msgref, 
		     Octstr *state, List *flag_cmds);
int mms_mmbox_delmsg(char *mmbox_root, char *user, Octstr *msgref);
List *mms_mmbox_search(char *mmbox_root, char *user,
		       List *state, List *flag_cmds, int start, int limit, 
		       List *msgrefs);

MmsMsg *mms_mmbox_get(char *mmbox_root, char *user,  Octstr *msgref, unsigned long *msize);
int mms_mmbox_count(char *mmbox_root, char *user,  unsigned long *msgcount, 
		    unsigned long *byte_count);

#define USER_MMBOX_MSG_QUOTA 200000 /* Means nothing at the moment. */
#define USER_MMBOX_DATA_QUOTA 2000000 /* ditto. */

#endif

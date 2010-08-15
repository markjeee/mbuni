/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * User-Agent profiles handling, content adaptation.
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_UAPROFILE_INCLUDED__
#define __MMS_UAPROFILE_INCLUDED__
#include "gwlib/gwlib.h"
#include "mms_msg.h"

#define PRES_TYPE "application/smil"

typedef struct MmsUaProfile  MmsUaProfile;

/* Start/stop profile engine. 
 * Starts a thread. Loads cache contents first. 
 */
extern int mms_start_profile_engine(char *cache_dir);
extern int mms_stop_profile_engine(void);

/* Gets the profile. Returns NULL if not cached or not existent. */
extern MmsUaProfile *mms_get_ua_profile(char *url);

/* Make a UA Profile out of the Accept HTTP headers. */
extern MmsUaProfile *mms_make_ua_profile(List *req_headers);

/* Transform Mms Message using the profile supplied. 
 * Returns 0 on success (and sets outmsg accordingly)
 * -1 if profile cannot be fetched (e.g. temporarily)
 * -2 if profile exists and UA does not support MMS.
 * If returns 0 and sets outmsg to NULL then transformed message was 
 * too large for recipient (as per profile).
 */
extern int mms_transform_msg(MmsMsg *inmsg, MmsUaProfile *prof, MmsMsg **outmsg);

/* Transforms the mms into a more 'readable' format:
 * SMIL is turned into html (with an alternative of text) if trans_smil is set,
 * image and audio changed to email preferred formats. Does not base64 mime parts of result!
 */
extern int mms_format_special(MmsMsg *inmsg,
			       int trans_smil, 
			       char *txtmsg, 
			      char *htmlmsg, MIMEEntity **outmsg);

extern unsigned long mms_ua_maxmsgsize(MmsUaProfile *prof);
#define DEFAULT_CHARSET "UTF-8"
#endif

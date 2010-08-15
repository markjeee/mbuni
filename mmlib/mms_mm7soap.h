/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MM7/SOAP message encoder/decoder and helper functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_MM7SOAP_INCLUDED__
#define __MMS_MM7SOAP_INCLUDED__
#include "mms_util.h"

#define MM7_SOAP_OK 1000
#define MM7_SOAP_FORMAT_CORRUPT 2007
#define MM7_SOAP_COMMAND_REJECTED 3005
#define MM7_SOAP_UNSUPPORTED_OPERATION 4003
#define MM7_SOAP_STATUS_OK(e) (((e) / 1000) == 1)
#define MM7_SOAP_CLIENT_ERROR(e) (((e) / 1000) == 2)

#if 0
#define MM7_DEFAULT_VERSION MMS_3GPP_VERSION
#define MM7_VERSION "5.3.0"
#endif

#define DEFAULT_MM7_VERSION MAKE_VERSION(5,3,0)

typedef struct MSoapMsg_t MSoapMsg_t;

typedef struct MM7Version_t {
     int major, minor1, minor2; /* e.g. 5.1.0. */
     int use_mm7_namespace;  /* whether to put in the namespace prefix. */
     char xmlns[128];     
} MM7Version_t;

/* Parse SOAP message given http headers and body. */
extern MSoapMsg_t *mm7_parse_soap(List *headers, Octstr *body);

/* Convert SOAP message to http headers and body. */
extern int mm7_soapmsg_to_httpmsg(MSoapMsg_t *m, MM7Version_t *ver, List **hdrs, Octstr **body);

/* Convert SOAP message to an MMS message. */
extern MmsMsg *mm7_soap_to_mmsmsg(MSoapMsg_t *m, Octstr *from);

/* Return the message type. */
extern int mm7_msgtype(MSoapMsg_t *m);

/* Collect and return envelope data: 
 * - to -- list of recipients to send to 
 * - subject -- subject
 * - msgid -- message id
 * - vasid/vaspid -- vasid and vaspid 
 */
extern int mm7_get_envelope(MSoapMsg_t *m, Octstr **sender,
			    List **to, Octstr **subject, 
			    Octstr **vasid, 
			    time_t *expiry_t, 
			    time_t *delivery_t,
			    Octstr **uaprof,
			    time_t *uaprof_tstamp);

/* Delete the thingie... */
extern void mm7_soap_destroy(MSoapMsg_t *m);

/* Convert a message to a SOAP message. */
MSoapMsg_t *mm7_mmsmsg_to_soap(MmsMsg *msg, Octstr *from, List *xto, 
			       Octstr *transid, Octstr *srvcode,
			       Octstr *linkedid,
			       int isclientside, 
			       char *vaspid, char *vasid, 
			       Octstr *uaprof,
			       time_t uaprof_tstamp,
			       List *extra_hdrs);
MSoapMsg_t *mm7_make_resp(MSoapMsg_t *mreq, int status, Octstr *msgid, int isclientside);
/* Return the header value for some header. */
Octstr *mm7_soap_header_value(MSoapMsg_t *m, Octstr *header);
#endif

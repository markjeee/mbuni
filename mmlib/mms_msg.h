/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS message encoder/decoder and helper functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#ifndef __MMS_MSG_INCLUDED__
#define __MMS_MSG_INCLUDED__
#include "gwlib/gwlib.h"
#include "wap/wsp_headers.h"
#include "gwlib/mime.h"


#include "mms_strings.h"

typedef struct MmsMsg MmsMsg; /* Opaque type. */

typedef MmsMsg *MmsMsgGetFunc_t(void *p1, void *p2, Octstr *msgref, unsigned long *msize);

extern int mms_validate_address(Octstr *s);

/* 
 * mms_frombinary: Parse MMS binary representation, return a Message or NULL.
 * errors are reported in errors string.
 * Takes from param which is put in if insert-address is requested.
 * NOTE: function may modify 'msg' argument as it parses. 
 */
extern MmsMsg *mms_frombinary_ex(Octstr *msg, Octstr *from, char *unified_prefix, List *strip_prefixes);

#define mms_frombinary(msg, from) mms_frombinary_ex((msg), (from), NULL, NULL)

/*
 * mms_tobinary: Return binary encoded Mms message
 */
extern Octstr *mms_tobinary(MmsMsg *msg);

extern mms_encoding mms_message_enc(MmsMsg *msg);

extern int mms_messagetype(MmsMsg *msg);

/* 
 * Convert Mms Message to standard Mime entity. 
 * Does base64 encoding of binary parts if base64 is true
 */
extern MIMEEntity *mms_tomime(MmsMsg *msg, int base64);
/*
 * De-convert from mime.
 */
extern  MmsMsg *mms_frommime(MIMEEntity *mime);

extern void mms_msgdump(MmsMsg *m, int headers_only);
extern void mms_destroy(MmsMsg *msg);

/* Make a delivery report message. */
extern MmsMsg *mms_deliveryreport(Octstr *msgid, Octstr *from, 
				  Octstr *to, time_t date, Octstr *status);

MmsMsg *mms_readreport(Octstr *msgid, Octstr *from, Octstr *to, time_t date, Octstr *status);

/* Return message headers. */
extern List *mms_message_headers(MmsMsg *msg);

/* Make a notification message out of this one and the url given. */
extern MmsMsg *mms_notification(Octstr *from, Octstr *subject, 
				Octstr *mclass, unsigned int msize, 
				Octstr *url,
				Octstr *transactionid, time_t expiryt, int optimizesize);

MmsMsg *mms_notifyresp_ind(char *transid, int menc, char *status, int report_allowed);
MmsMsg *mms_retrieveconf(MmsMsg *msg, Octstr *transactionid, char *err, char *errtxt, Octstr *opt_from, int menc);
int mms_remove_headers(MmsMsg *m, char *name);
MmsMsg *mms_sendconf(char *errstr, char *msgid, char *transid, int isforward, int menc);


Octstr *mms_get_header_value(MmsMsg *msg, Octstr  *header);

/* Returns a list of values for the header given. */
List *mms_get_header_values(MmsMsg *msg, Octstr  *header);

int mms_replace_header_value(MmsMsg *msg, char *hname, char *value);
int mms_replace_header_values(MmsMsg *msg, char *hname, List *values);

/* Get headers from 'headers' that are not already in message headers and add them. */
int mms_add_missing_headers(MmsMsg *msg, List *headers);

int mms_convert_readrec2readorig(MmsMsg *msg);


MmsMsg *mms_storeconf(char *errstr, char *transid, Octstr *msgloc, int isupload, int menc);
MmsMsg *mms_deleteconf(int menc, char *transid);

/* Makes a view-conf message. Insane number of arguments I know, 
 * but it seems a fair price to keep the interfaces clean
 *
 */
MmsMsg *mms_viewconf(char *transid, 
		     List *msgrefs, 
		     List *msglocs,
		     char *err,
		     List *required_headers,
		     MmsMsgGetFunc_t *getmsg, 
		     void *p1, void *p2,		     
		     int maxsize, int menc,
		     List *otherhdrs);

/* Returns a copy of the message body. Either as list of  MIME Entities or as string -- 
 * caller must consult content_type to determine which is returned. 
 */
void *mms_msgbody(MmsMsg *msg);

/* Remove message body. */
int mms_clearbody(MmsMsg *msg);

/* Put a new message body. Old one removed if it is there. 
 * second param is a list of MIME entities or a string  depending on content type
 * ismultipart tells us which. No copy is made of body. 
 * Use function with extreme caution! It should be for testing only really!!!!
 */
int mms_putbody(MmsMsg *msg, void *body, int ismultipart);

/* Convert a retrieve_conf message to a send_req messagre 
 * checked runtime error to pass anything but a send_req/retrieve_conf.
 * returns 0 on success.
 */
int mms_make_sendreq(MmsMsg *retrieveconf);
#endif

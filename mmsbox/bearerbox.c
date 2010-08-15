/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSC handler functions: Receive and send MMS messages to MMSCs 
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <sys/file.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <dlfcn.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mmsbox.h"
#include "mms_queue.h"

#include "mmsbox.h"

#define MOD_SUBJECT(msg, mmc,xfrom) do {				\
	  if ((mmc)->reroute_mod_subject) {				\
	       Octstr *s = mms_get_header_value((msg),octstr_imm("Subject")); \
	       Octstr *f = octstr_duplicate(xfrom);			\
	       int _i;							\
	       if (s == NULL) s = octstr_create("");			\
	       if ((_i = octstr_search(f, octstr_imm("/TYPE="), 0)) >= 0) \
		    octstr_delete(f, _i, octstr_len(f));		\
	       octstr_format_append(s, " (from %S)", (f));		\
	       mms_replace_header_value((msg), "Subject", octstr_get_cstr(s)); \
	       octstr_destroy(s); octstr_destroy(f);			\
	  }								\
     } while(0)




static int auth_check(Octstr *user, Octstr *pass, List *headers, int *has_auth_hdr)
{
     int i, res = -1;
     Octstr *v = http_header_value(headers, octstr_imm("Authorization"));
     Octstr *p = NULL, *q = NULL;

     *has_auth_hdr = (v != NULL);
     if (octstr_len(user) == 0) {
	  res = 0;
	  goto done;
     }

     if (!v ||
	 octstr_search(v, octstr_imm("Basic "), 0) != 0)
	  goto done;
     p = octstr_copy(v, sizeof "Basic", octstr_len(v));
     octstr_base64_to_binary(p);
     
     i = octstr_search_char(p, ':', 0);
     q = octstr_copy(p, i+1, octstr_len(p));
     octstr_delete(p, i, octstr_len(p));
     
     /* p = user, q = pass. */

     if (octstr_compare(user, p) != 0 ||
	 octstr_compare(pass, q) != 0)
	  res = -1;
     else 
	  res = 0;
done:
     octstr_destroy(v);
     octstr_destroy(p);     
     octstr_destroy(q);
     return res;
}

static Octstr *get_dlr_notify_url(Octstr *msgid, char *report_type, Octstr *mmc_gid, Octstr *mmc_id,
				  Octstr *status,
				  Octstr **transid)
{

     Octstr *xtransid = NULL, *url = NULL;

     mms_dlr_url_get(msgid, report_type, mmc_gid, &url, &xtransid);

     if (transid)
	  *transid = xtransid;
     else 
	  octstr_destroy(xtransid);
     
     if (octstr_len(url) == 0) {
	  if (url)
	       mms_info(0, "MM7", NULL, 
			"Sending delivery-report skipped: `url' is empty, `group_id'=[%s], `msgid'=[%s]",
			octstr_get_cstr(mmc_gid), octstr_get_cstr(msgid));
	  octstr_destroy(url); 
	  url = NULL;
	  goto done;
     } else if (octstr_search(url, octstr_imm("msgid:"), 0) == 0) { /* a fake one, skip it. */
	  octstr_destroy(url); 
	  url = NULL;
	  
	  goto done;
     }
#if 0
     /* At what point do we delete it? For now, when we get a read report, 
      * and also when we get  a delivery report that is not 'deferred' or sent or forwarded
      */

     if (strcmp(report_type, "read-report") == 0 ||
	 (octstr_case_compare(status, octstr_imm("Deferred")) != 0 &&
	  octstr_case_compare(status, octstr_imm("Forwarded")) != 0))
	  mms_dlr_url_remove(msgid, report_type, mmc_gid);
#endif
done:

     return url;
}


static void fixup_relayed_report(MmsMsg *m, MmscGrp *mmc, char *rtype, Octstr *status)
{
     
     Octstr *value = mms_get_header_value(m, octstr_imm("Message-ID")); 
     Octstr *newmsgid = NULL, *transid = NULL;
     
     /* Firstly, take care to look for the record we saved, and re-write the MessageID. */
     if (value && 
	 mms_dlr_url_get(value, rtype, mmc->group_id, &newmsgid, &transid) == 0) {
	  int x = octstr_search_char(newmsgid, ':', 0);
	  
	  if (x>=0) 
	       octstr_delete(newmsgid, 0, x+1);
	  
	  mms_replace_header_value(m, "Message-ID", octstr_get_cstr(newmsgid));
	  /* Add it back as original. */
	  mms_replace_header_value(m, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value));

#if 0	
	  if (strcmp(rtype, "read-report") == 0 ||
	      (octstr_case_compare(status, octstr_imm("Deferred")) != 0 &&
	       octstr_case_compare(status, octstr_imm("Forwarded")) != 0))	       
	       mms_dlr_url_remove(value, rtype, mmc->group_id); /* only remove if not 
									     * interim status 
									     */
#endif
     }
     octstr_destroy(newmsgid);
     octstr_destroy(transid);
     octstr_destroy(value);
}

/* returns the DLR/RR URL, fills in the queue header info. */
Octstr *mmsbox_get_report_info(MmsMsg *m, MmscGrp *mmsc, Octstr *out_mmc_id, char *report_type, 
			       Octstr *status, List *qhdr, Octstr *uaprof, 
			       time_t uaprof_tstamp, 
			       Octstr *msgid)
{
     Octstr *res;

     if (mmsc == NULL)
	  res = NULL;
     else if (out_mmc_id != NULL) { /* internal routing. */      
	  if (m) 
	       fixup_relayed_report(m, mmsc, report_type, octstr_imm(""));
	  res = NULL;
     } else {
	  Octstr *transid = NULL;
	  
	  res = get_dlr_notify_url(msgid, report_type,mmsc->group_id, mmsc->id, 
				   status, &transid);
	  http_header_add(qhdr, "X-Mbuni-Mmsc-GroupID", octstr_get_cstr(mmsc->group_id));
	  
	  if (transid) {		    
	       http_header_add(qhdr, "X-Mbuni-TransactionID", octstr_get_cstr(transid));		    
	       octstr_destroy(transid);
	  }
	       
	  if (uaprof) {
	       Octstr *sx = date_format_http(uaprof_tstamp);
	       http_header_add(qhdr, "X-Mbuni-UAProf", octstr_get_cstr(uaprof));
	       http_header_add(qhdr, "X-Mbuni-Timestamp", octstr_get_cstr(sx));
	       octstr_destroy(sx);
	  }	  
     }

     return res;
}

/* These functions are very similar to those in mmsproxy */
static int mm7soap_receive(MmsBoxHTTPClientInfo *h)
{

     MSoapMsg_t *mreq = NULL, *mresp = NULL;
     int hstatus = HTTP_OK;
     List *rh = NULL;
     Octstr *reply_body = NULL;
     
     List *to = NULL;
     Octstr *from = NULL, *subject = NULL,  *vasid = NULL, *msgid = NULL, *uaprof = NULL;
     time_t expiryt = -1, delivert = -1, uaprof_tstamp = -1;
     MmsMsg *m = NULL;
     int status = 1000;
     unsigned char *msgtype = (unsigned char *)"";
     Octstr *qf = NULL, *mmc_id = NULL, *qdir = NULL;
     List *qhdr = http_create_empty_headers();

     if (h->body)     
	  mreq = mm7_parse_soap(h->headers, h->body);
     if (mreq)
	  msgtype = mms_mm7tag_to_cstr(mm7_msgtype(mreq));
     debug("mmsbox.mm7sendinterface", 0,
	   " --> Enterred mm7dispatch interface, mreq=[%s] mtype=[%s] <-- ",
	   mreq ? "Ok" : "Null",
	   mreq ? (char *)msgtype : "Null");
          
     if (!mreq) {
	  mresp = mm7_make_resp(NULL, MM7_SOAP_FORMAT_CORRUPT, NULL,1);
	  status = 4000;
	  goto done;
     } 

     mm7_get_envelope(mreq, &from, &to, &subject, &vasid, 
		      &expiryt, &delivert, &uaprof, &uaprof_tstamp);
     
     if (!from)
	  from = octstr_create("anon@anon");
     
     qdir = get_mmsbox_queue_dir(from, to, h->m, &mmc_id); /* get routing info. */

     switch (mm7_msgtype(mreq)) {
	  Octstr *value, *value2;

     case MM7_TAG_DeliverReq:
	  m = mm7_soap_to_mmsmsg(mreq, from); 
	  if (m) {
	       /* Store linked id so we use it in response. */
	       Octstr *linkedid = mm7_soap_header_value(mreq, octstr_imm("LinkedID"));
	       List *qh = http_create_empty_headers();
	       int dlr;
	       
	       value = mms_get_header_value(m, octstr_imm("X-Mms-Delivery-Report"));	  

	       if (value && 
		   octstr_case_compare(value, octstr_imm("Yes")) == 0) 
		    dlr = 1;
	       else 
		    dlr = 0;
	       
	       if (delivert < 0)
		    delivert = time(NULL);
	       
	       if (expiryt < 0)
		    expiryt = time(NULL) + DEFAULT_EXPIRE;
	       
	       if (uaprof) {
		    Octstr *sx = date_format_http(uaprof_tstamp);
		    http_header_add(qh, "X-Mbuni-UAProf", octstr_get_cstr(uaprof));
		    http_header_add(qh, "X-Mbuni-Timestamp", octstr_get_cstr(sx));
		    octstr_destroy(sx);
	       }

	       MOD_SUBJECT(m, h->m, from);
	       
	       qf = qfs->mms_queue_add(from, to, subject, 
				       h->m->id, mmc_id,
				       delivert, expiryt, m, linkedid, 
				       NULL, NULL, 
				       NULL, NULL,
				       qh,
				       dlr, 
				       octstr_get_cstr(qdir),
				       "MM7/SOAP-IN",
				       NULL);

	       if (qf == NULL)  {
		    status = 4000; 
		    mms_error(0, "MM7", h->m->id,
			      "Failed to write queue entry for received MM7/SOAP DeliverReq message from mmc=%s to MMS Message!",
			      octstr_get_cstr(h->m->id));
	       } else {
		    
		    msgid = mms_make_msgid(octstr_get_cstr(qf), NULL);
		    mms_log("Received", from, to, -1, msgid, NULL, h->m->id, "MMSBox", 
			    h->ua, NULL);
	       }
	       
	       octstr_destroy(linkedid);
	       octstr_destroy(value);	      
	       http_destroy_headers(qh);
	  }  else {
	       mms_error(0, "MM7", h->m->id,
		     "Failed to convert received MM7/SOAP DeliverReq message from mmc=%s to MMS Message!",
		       octstr_get_cstr(h->m->id));
	       status = 4000;	  
	  }
	  mresp = mm7_make_resp(mreq, status, NULL,1);

	  break; 	  
	  
     case MM7_TAG_DeliveryReportReq:
	  value = mm7_soap_header_value(mreq, octstr_imm("MMStatus"));
	  msgid = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
	  
	  if ((value2 = mm7_soap_header_value(mreq, octstr_imm("StatusText"))) != NULL) {
	       
	       http_header_add(qhdr, "X-Mbuni-StatusText", octstr_get_cstr(value2));
	       octstr_destroy(value2);
	       value2 = NULL;
	  }

	  if ((value2 = mm7_soap_header_value(mreq, octstr_imm("Details"))) != NULL) {
	       
	       http_header_add(qhdr, "X-Mbuni-StatusDetails", octstr_get_cstr(value2));
	       octstr_destroy(value2);
	       value2 = NULL;
	  }

	  m = mm7_soap_to_mmsmsg(mreq, from); 
	  value2 = mmsbox_get_report_info(m, h->m, mmc_id, "delivery-report", 
					  value, qhdr, uaprof, uaprof_tstamp, msgid);
	  qf = qfs->mms_queue_add(from, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  value2, NULL,
				  qhdr,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/SOAP-IN",
				  NULL);
	  if (qf)  
	       /* Log to access log */
	       mms_log("Received DLR", from, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);
	  else 
		    status = 4000;
	  mresp = mm7_make_resp(mreq, status, NULL,1);

	  octstr_destroy(value);
	  octstr_destroy(value2);
	  break;
     
     case MM7_TAG_ReadReplyReq:

	  m = mm7_soap_to_mmsmsg(mreq, from); 
	  value = mm7_soap_header_value(mreq, octstr_imm("MMStatus"));
	  msgid = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
	  
	  value2 = mmsbox_get_report_info(m, h->m, mmc_id, "read-report", value, qhdr, uaprof, uaprof_tstamp, msgid);

	  qf = qfs->mms_queue_add(from, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  value2, NULL,
				  qhdr,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/SOAP-IN",
				  NULL);
	  if (qf)  
	       /* Log to access log */
	       mms_log("Received RR", from, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);		    
	  else 
	       status = 4000;
	  
	  mresp = mm7_make_resp(mreq, status, NULL,1);

	  octstr_destroy(value);
	  octstr_destroy(value2);
	  break;
	  
     default:
	  mresp = mm7_make_resp(mreq, MM7_SOAP_UNSUPPORTED_OPERATION, NULL,1);
	  status = MM7_SOAP_UNSUPPORTED_OPERATION;
	  break;	  
     }
     
 done:
     if (mresp && mm7_soapmsg_to_httpmsg(mresp, &h->m->ver, &rh, &reply_body) == 0) 
	  http_send_reply(h->client, hstatus, rh, reply_body);
     else 
	  http_close_client(h->client);

     debug("mmsbox.mm7sendinterface", 0,
	   " --> leaving mm7dispatch interface, mresp=[%s], body=[%s], mm7_status=[%d] <-- ",
	   mresp ? "ok" : "(null)",
	   reply_body ? "ok" : "(null)", status);
     
     octstr_destroy(from);     
     octstr_destroy(subject);
     octstr_destroy(vasid);
     octstr_destroy(msgid);
     octstr_destroy(qf);
     octstr_destroy(uaprof);
     mms_destroy(m);
     http_destroy_headers(rh);
     octstr_destroy(reply_body);
     mm7_soap_destroy(mresp);
     mm7_soap_destroy(mreq);
     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);     
     octstr_destroy(mmc_id);
     http_destroy_headers(qhdr);

     return MM7_SOAP_STATUS_OK(status) ? 0 : -1;
}


/* helper function for queueing delivery reports. */
static int queue_dlr(MmscGrp *mmc, Octstr *from, Octstr *to, Octstr *msgid, Octstr *status, char *interf, List *errl)
{
     Octstr *mmc_id = NULL, *qdir;
     MmsMsg *m = mms_deliveryreport(msgid, from, to, time(NULL), status);
     List *lto = gwlist_create();
     int ret;
     Octstr *qf, *rr_uri = NULL;
     List *rqh = http_create_empty_headers(); 

     
     if (errl) 
	  http_header_combine(rqh, errl); /* add status stuff. */

     
     gwlist_append(lto, octstr_duplicate(to));

     
     qdir = get_mmsbox_queue_dir(from, lto, mmc, &mmc_id); /* get routing info. */

     rr_uri = mmsbox_get_report_info(m, mmc, mmc_id, "delivery-report", status, rqh, NULL, 0, msgid);     

     qf = qfs->mms_queue_add(from, lto, NULL, 
			     mmc ? mmc->id : NULL, mmc_id,
			     0, time(NULL) + default_msgexpiry, m, NULL, 
			     NULL, NULL,
			     rr_uri, NULL,
			     rqh,
			     0,
			     octstr_get_cstr(qdir), 				  
			     interf,
			     NULL);
     if (qf)  {
	  /* Log to access log */
	  mms_log("Received DLR", from, lto, -1, msgid, status, mmc ? mmc->id : NULL, "MMSBox", NULL, NULL);
	  ret = 0;
     }  else 
	  ret = -1;

     octstr_destroy(qf);
     http_destroy_headers(rqh);     
     octstr_destroy(rr_uri);
     
     gwlist_destroy(lto, (void *)octstr_destroy);
     octstr_destroy(mmc_id);
     mms_destroy(m);

     return ret;
}

static int mm7eaif_receive(MmsBoxHTTPClientInfo *h)
{
     MmsMsg *m = NULL;
     List *mh = NULL;
     int hstatus = HTTP_NO_CONTENT;
     List *rh = http_create_empty_headers();
     List *rqh = http_create_empty_headers(); 
     Octstr *reply_body = NULL, *value = NULL, *value2 = NULL;
     
     List *to = gwlist_create(), *hto = NULL;
     Octstr *subject = NULL,  *otransid = NULL, *msgid = NULL;
     Octstr *hfrom = NULL, *rr_uri = NULL;
     time_t expiryt = -1, deliveryt = -1;
     Octstr *qf = NULL, *xver = NULL, *mmc_id = NULL, *qdir = NULL;
     int msize = h->body ? octstr_len(h->body) : 0;
     int dlr;
     int mtype;
     
     debug("mmsbox.mm7eaif.sendinterface", 0, 
	   " --> Enterred eaif send interface, blen=[%d] <--- ", 
	   msize);

     hfrom = http_header_value(h->headers, octstr_imm("X-NOKIA-MMSC-From"));     
     if (!h->body ||  /* A body is required, and must parse */
	 (m = mms_frombinary(h->body, hfrom ? hfrom : octstr_imm("anon@anon"))) == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Unexpected MMS message, no body?");
	  
	  goto done;
     }      

     /* XXXX handle delivery reports differently. */
     mtype = mms_messagetype(m);
     mh = mms_message_headers(m);
     /* Now get sender and receiver data. 
      * for now we ignore adaptation flags. 
      */
     mms_collect_envdata_from_msgheaders(mh, &to, &subject, 
					 &otransid, &expiryt, &deliveryt, 
					 DEFAULT_EXPIRE,
					 -1,
					 octstr_get_cstr(unified_prefix), 
					 strip_prefixes);
     
     
     if ((hto = http_header_find_all(h->headers, "X-NOKIA-MMSC-To")) != NULL && 
	 gwlist_len(hto) > 0) { /* To address is in headers. */
	  int i, n;
	  
	  gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
	  to = gwlist_create();
	  for (i = 0, n = gwlist_len(hto); i < n; i++) {
	       Octstr *h = NULL, *v = NULL;
	       List *l;
	       void *x;
	       
	       http_header_get(hto,i,  &h, &v);	       
	       l = http_header_split_value(v);
	       
	       while ((x = gwlist_extract_first(l)) != NULL)
		    gwlist_append(to, x);
	       
	       gwlist_destroy(l, NULL);
	       octstr_destroy(h);	       
	       octstr_destroy(v);	       	       
	  }
	  
     }
     
     qdir = get_mmsbox_queue_dir(hfrom, to, h->m, &mmc_id); /* get routing info. */
     
     switch(mtype) {
     case MMS_MSGTYPE_SEND_REQ:
     case MMS_MSGTYPE_RETRIEVE_CONF:
       
	  /* Get Message ID */
	  if ((msgid = http_header_value(h->headers, octstr_imm("X-NOKIA-MMSC-Message-Id"))) == NULL)
	       msgid = http_header_value(mh, octstr_imm("Message-ID"));	  
	  else 
	       mms_replace_header_value(m, "Message-ID", octstr_get_cstr(msgid)); /* replace it in the message.*/

	  value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"));	  
	  if (value && 
	      octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	       dlr = 1;
	  else 
	       dlr = 0;
	 
	  
	  if (deliveryt < 0)
	       deliveryt = time(NULL);
	  
	  if (expiryt < 0)
	       expiryt = time(NULL) + DEFAULT_EXPIRE;
	  
	  if (hfrom == NULL)
	       hfrom = http_header_value(mh, octstr_imm("From"));
	  
	  mms_remove_headers(m, "Bcc");
	  mms_remove_headers(m, "X-Mms-Delivery-Time");
	  mms_remove_headers(m, "X-Mms-Expiry");
	  mms_remove_headers(m, "X-Mms-Sender-Visibility");
	  
	  MOD_SUBJECT(m, h->m, hfrom);

	  /* Save it,  put message id in header, return. */     
	  qf = qfs->mms_queue_add(hfrom, to, subject, 
				  h->m->id, mmc_id,
				  deliveryt, expiryt, m, NULL, 
				  NULL, NULL,
				  NULL, NULL,
				  NULL,
				  dlr,
				  octstr_get_cstr(qdir),
				  "MM7/EAIF-IN",
				  NULL);
	  
	  if (qf) {
	       /* Log to access log */
	       mms_log("Received", hfrom, to, msize, msgid, NULL, h->m->id, "MMSBox", h->ua, NULL);
	       
	       hstatus = HTTP_NO_CONTENT;
	  } else 
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;

	  octstr_destroy(value);
	  octstr_destroy(value2);	  
	  break;
     case MMS_MSGTYPE_DELIVERY_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 


	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "delivery-report", 
					  value, rqh, NULL, 0, msgid);
	  if (value2 && mmc_id == NULL)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2)); 
	  
	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/EAIF-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("DeliveryReport", hfrom, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);
	       
	       hstatus = HTTP_NO_CONTENT;
	  }  else 
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	  octstr_destroy(value);
	  octstr_destroy(value2);
	  break;
	  
     case MMS_MSGTYPE_READ_ORIG_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Read-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 

	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "read-report", 
					  value, rqh, NULL, 0, msgid);
	  if (value2 && mmc_id == NULL)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2)); 
	  
	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/EAIF-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("Received RR", hfrom, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);    
	       hstatus = HTTP_NO_CONTENT;
	  }  else 
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;	  

	  octstr_destroy(value);
	  octstr_destroy(value2);
	  break;
     }

 done:
     
     xver = octstr_format(EAIF_VERSION, h->m->ver.major, h->m->ver.minor1);
     http_header_add(rh, "X-NOKIA-MMSC-Version", octstr_get_cstr(xver));
     octstr_destroy(xver);

     http_send_reply(h->client, hstatus, rh, octstr_imm(""));

     http_destroy_headers(hto);     
     http_destroy_headers(rqh);     
     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
     octstr_destroy(hfrom);     
     octstr_destroy(subject);
     octstr_destroy(otransid);
     octstr_destroy(msgid);
     octstr_destroy(qf);
     octstr_destroy(mmc_id);
     octstr_destroy(rr_uri);

     http_destroy_headers(mh);
     mms_destroy(m);      

     return http_status_class(hstatus) == HTTP_STATUS_SUCCESSFUL ? 0 : -1;
}

static int mm7http_receive(MmsBoxHTTPClientInfo *h)
{
     MmsMsg *m = NULL;
     List *mh = NULL;
     int hstatus = HTTP_OK;
     List *rh = http_create_empty_headers();
     Octstr *reply_body = NULL;
     
     List *to = NULL;
     Octstr *hto = NULL, *subject = NULL,  *msgid = NULL;
     Octstr *hfrom = NULL, *body, *rr_uri = NULL, *dlr_uri = NULL;
     time_t expiryt = -1, deliveryt = -1;
     Octstr *qf = NULL, *mmc_id = NULL, *qdir = NULL, *s;
     int msize;
     int dlr, rr;
     int mtype;
     List *cgivars_ctypes = NULL, *rqh = http_create_empty_headers();

     parse_cgivars(h->headers, h->body, &h->cgivars, &cgivars_ctypes);
     
     hfrom = http_cgi_variable(h->cgivars, "from");     
     hto =  http_cgi_variable(h->cgivars, "to");     
     body = http_cgi_variable(h->cgivars, "mms");

     msize = octstr_len(body);

     debug("mmsbox.mm7http.sendinterface", 0, 
	   " --> Enterred http-mmsc send interface, blen=[%d] <--- ", 
	   msize);
     
     if (hto == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Missing 'to' argument");
	  
	  goto done;

     } else if (hfrom == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Missing 'from' argument");
	  
	  goto done;
	  
     } else if (body == NULL ||  /* A message is required, and must parse */
		(m = mms_frombinary(body, hfrom ? hfrom : octstr_imm("anon@anon"))) == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Unexpected MMS message, no content?");
	  
	  goto done;
     }      


     to = octstr_split_words(hto);

     mtype = mms_messagetype(m);
     mh = mms_message_headers(m);

     /* find interesting headers. */
     subject = http_header_value(mh, octstr_imm("Subject"));

     /* Find expiry and delivery times */
     
     if ((s = http_header_value(mh, octstr_imm("X-Mms-Expiry"))) != NULL) {
	  expiryt = date_parse_http(s);
	  octstr_destroy(s);
     } else 
	  expiryt = time(NULL) +  DEFAULT_EXPIRE;
          
     if ((s = http_header_value(mh, octstr_imm("X-Mms-Delivery-Time"))) != NULL) {
	  deliveryt = date_parse_http(s);
	  octstr_destroy(s);
     } else 
	  deliveryt = 0;
     
     qdir = get_mmsbox_queue_dir(hfrom, to, h->m, &mmc_id); /* get routing info. */
     
     switch(mtype) {
	  Octstr *value, *value2;
     case MMS_MSGTYPE_SEND_REQ:
     case MMS_MSGTYPE_RETRIEVE_CONF:
       
	  /* Get/make a Message ID */
	  if ((msgid = mms_get_header_value(m, octstr_imm("Message-ID"))) == NULL) { /* Make a message id for it directly. We need it below. */
	       msgid = mms_make_msgid(NULL, NULL);
	       mms_replace_header_value(m, "Message-ID", octstr_get_cstr(msgid));	       
	  }
	  
	  if ((value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"))) != NULL && 
	      octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	       dlr = 1;
	  else 
	       dlr = 0;
	  octstr_destroy(value);

	  if ((value = http_header_value(mh, octstr_imm("X-Mms-Read-Report"))) != NULL && 
	      octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	       rr = 1;
	  else 
	       rr = 0;
	  octstr_destroy(value);
	  
	  if (deliveryt < 0)
	       deliveryt = time(NULL);
	  
	  if (expiryt < 0)
	       expiryt = time(NULL) + DEFAULT_EXPIRE;
	  
	  mms_remove_headers(m, "Bcc");
	  mms_remove_headers(m, "X-Mms-Delivery-Time");
	  mms_remove_headers(m, "X-Mms-Expiry");
	  mms_remove_headers(m, "X-Mms-Sender-Visibility");
	  
	  MOD_SUBJECT(m, h->m, hfrom);
	  

	  if (qdir == outgoing_qdir) { /* We need to remember the old message ID so we can re-write it 
				   * if a DLR is relayed backwards. 			
				   */
	       Octstr *t = mms_maketransid(NULL, octstr_imm(MM_NAME)); /* make a fake transaction id  so dlr works*/

	       http_header_add(rqh, "X-Mbuni-TransactionID", octstr_get_cstr(t));
	       if (dlr)
		    dlr_uri = octstr_format("msgid:%S", msgid);
	       if (rr)
		    rr_uri  =  octstr_format("msgid:%S", msgid); 	       	 

	       octstr_destroy(t);
	  }

	  /* Save it,  put message id in header, return. */     
	  qf = qfs->mms_queue_add(hfrom, to, subject, 
				  h->m->id, mmc_id,
				  deliveryt, expiryt, m, NULL, 
				  NULL, NULL,
				  dlr_uri, rr_uri,
				  rqh,
				  dlr,
				  octstr_get_cstr(qdir),
				  "MM7/HTTP-IN",
				  NULL);
	  
	  if (qf) {
	       /* Log to access log */
	       mms_log("Received", hfrom, to, msize, msgid, NULL, h->m->id, "MMSBox", h->ua, NULL);
	       
	       hstatus = HTTP_OK;
	  } else 
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	  break;
     case MMS_MSGTYPE_DELIVERY_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 
	  
	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "delivery-report", 
					  value, rqh, NULL, 0, msgid);
	  if (mmc_id == NULL && value2)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2));		    

	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/HTTP-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("DeliveryReport", hfrom, to, -1, msgid,value, h->m->id, "MMSBox", h->ua, NULL);
	       
	       hstatus = HTTP_OK;
	  }  else 
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	  octstr_destroy(value);
	  octstr_destroy(value2);
	  break;
	  
     case MMS_MSGTYPE_READ_ORIG_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Read-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 

	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "read-report", 
					  value, rqh, NULL, 0, msgid);
	  if (mmc_id == NULL && value2)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2));		    

	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/HTTP-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("Received RR", hfrom, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);		    
	       hstatus = HTTP_NO_CONTENT;
	  }  else 
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	  octstr_destroy(value);
	  octstr_destroy(value2);
	  break;
     }
     
 done:
     
     http_header_add(rh, "X-Mbuni-Version", VERSION);
     
     http_send_reply(h->client, hstatus, rh, msgid ? msgid : (qf ? qf : octstr_imm("")));

     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);

     octstr_destroy(subject);

     octstr_destroy(qf);
     octstr_destroy(mmc_id);
     octstr_destroy(msgid);
     
     http_destroy_headers(mh);
     http_destroy_headers(rh);
     http_destroy_headers(rqh);

     if (m) 
	  mms_destroy(m);      
     
     http_destroy_cgiargs(cgivars_ctypes);
     
     return http_status_class(hstatus) == HTTP_STATUS_SUCCESSFUL ? 0 : -1;
}


static void dispatch_mm7_recv(List *rl) 
{

     MmsBoxHTTPClientInfo *h;
     
     while ((h = gwlist_consume(rl)) != NULL) {
	  int ret = -1, has_auth = 0;
	  MmscGrp *m = h->m;
	  if (auth_check(m->incoming.user, 
			 m->incoming.pass, 
			 h->headers, &has_auth) != 0) { /* Ask it to authenticate... */
	       List *hh = http_create_empty_headers();
	       http_header_add(hh, "WWW-Authenticate", 
			       "Basic realm=\"" MM_NAME "\"");
	       http_send_reply(h->client, HTTP_UNAUTHORIZED, hh, 
			       octstr_imm("Authentication failed"));			   
	       http_destroy_headers(hh);
	       if (!has_auth)
		    mms_info_ex("auth",0, "MM7", m->id, "Auth failed, incoming connection, MMC group=[%s]",
				m->id ? octstr_get_cstr(m->id) : "(none)");
	       else 
		    mms_error_ex("auth",0, "MM7", m->id, "Auth failed, incoming connection, MMC group=[%s]",
				 m->id ? octstr_get_cstr(m->id) : "(none)");	       
	  } else if (h->m->type == SOAP_MMSC)
	       ret = mm7soap_receive(h);
	  else if (h->m->type == EAIF_MMSC)
	       ret = mm7eaif_receive(h);
	  else 
	       ret = mm7http_receive(h);

	  h->m->last_pdu = time(NULL);

	  if (ret == 0)
	       h->m->mo_pdus++;
	  else 
	       h->m->mo_errors++;
	  free_mmsbox_http_clientInfo(h, 1);
     }
}

void mmsc_receive_func(MmscGrp *m)
{
     int i;
     MmsBoxHTTPClientInfo h = {NULL};
     List *mmsc_incoming_reqs = gwlist_create();
     long *thids = gw_malloc((maxthreads + 1)*sizeof thids[0]);

     gwlist_add_producer(mmsc_incoming_reqs);
     
     for (i = 0; i<maxthreads; i++)
	  thids[i] = gwthread_create((gwthread_func_t *)dispatch_mm7_recv, mmsc_incoming_reqs);
     
     h.m = m;
     while(rstop == 0 &&
	   (h.client = http_accept_request(m->incoming.port, 
					   &h.ip, &h.url, &h.headers, 
					   &h.body, &h.cgivars)) != NULL) 
	  if (is_allowed_ip(m->incoming.allow_ip, m->incoming.deny_ip, h.ip)) {
	       MmsBoxHTTPClientInfo *hx = gw_malloc(sizeof hx[0]); 

	       h.ua = http_header_value(h.headers, octstr_imm("User-Agent"));	       

	       *hx = h;		    

	       debug("mmsbox", 0, 
		     " MM7 Incoming, IP=[%s], MMSC=[%s], dest_port=[%ld] ", 
		     h.ip ? octstr_get_cstr(h.ip) : "", 
		     octstr_get_cstr(m->id),
		     m->incoming.port);  
	      	       
	       /* Dump headers, url etc. */
#if 0
	       http_header_dump(h.headers);
	       if (h.body) octstr_dump(h.body, 0);
	       if (h.ip) octstr_dump(h.ip, 0);
#endif

	       gwlist_produce(mmsc_incoming_reqs, hx);	      
	  } else {
	       h.ua = http_header_value(h.headers, octstr_imm("User-Agent"));
	       
	       mms_error_ex("auth",0,  "MM7", m->id, "HTTP: Incoming IP denied MMSC[%s] ip=[%s], ua=[%s], disconnected",
		     m->id ? octstr_get_cstr(m->id) : "(none)", 
		     h.ip ? octstr_get_cstr(h.ip) : "(none)",
		     h.ua ? octstr_get_cstr(h.ua) : "(none)");
               
               http_send_reply(h.client, HTTP_FORBIDDEN, NULL,
                               octstr_imm("Access denied."));
	       free_mmsbox_http_clientInfo(&h, 0);
	  }
     
     debug("proxy", 0, "MMSBox: MM7 receiver [mmc=%s] Shutting down...", octstr_get_cstr(m->id));          
     gwlist_remove_producer(mmsc_incoming_reqs);

     for (i = 0; i<maxthreads; i++)
	  if (thids[i] >= 0)
	       gwthread_join(thids[i]);
     
     gwlist_destroy(mmsc_incoming_reqs, NULL);
     gw_free(thids);
     
     debug("proxy", 0, "MMSBox: MM7 receiver [mmc=%s] Shutting down complete.", octstr_get_cstr(m->id));
}


/* XXX Returns msgid in mmsc or NULL if error. Caller uses this for DLR issues. 
 * Caller must make sure throughput issues
 * are observed!
 * Don't remove from queue on fail, just leave it to expire. 
 */
static Octstr *mm7soap_send(MmscGrp *mmc, Octstr *from, Octstr *to, 
			    Octstr *transid,
			    Octstr *linkedid, 
			    char *vasid,
			    Octstr *service_code,
			    List *hdrs,
			    MmsMsg *m, Octstr **error,
			    List **errl,
			    int *retry)
{
     Octstr *ret = NULL;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK, tstatus  = -1;
     List *xto = gwlist_create();
     MSoapMsg_t *mreq = NULL, *mresp = NULL;
     List *rh = NULL, *ph = NULL;
     Octstr *body = NULL, *rbody = NULL, *url = NULL; 
     Octstr *s;
     char *xvasid = vasid ? vasid : (mmc->default_vasid ? octstr_get_cstr(mmc->default_vasid) : NULL);

     mms_info(0, "MM7", mmc->id,  "MMSBox: Send[soap] to MMSC[%s], msg type [%s], from %s, to %s", 
	  mmc->id ? octstr_get_cstr(mmc->id) : "", 
	  mms_message_type_to_cstr(mtype), 
	  octstr_get_cstr(from), octstr_get_cstr(to));    
     
     gwlist_append(xto, to);
     
     if ((mreq = mm7_mmsmsg_to_soap(m, (mmc == NULL || mmc->no_senderaddress == 0) ? from : NULL, 
				    xto, transid,
				    service_code, 
				    linkedid, 
				    1, octstr_get_cstr(mmc->vasp_id), xvasid, NULL, 0,/* UA N/A on this side. */
				    hdrs)) == NULL) {
	  *error = octstr_format("Failed to convert Msg[%S] 2 SOAP message!",
				 mms_message_type_to_string(mtype));
	  goto done1;
     }
     
     if (mm7_soapmsg_to_httpmsg(mreq, &mmc->ver, &rh, &body) < 0) {
	  *error = octstr_format("Failed to convert SOAP message to HTTP Msg!");
	  goto done1;
     } 

     if (hdrs)
	  http_header_combine(rh, hdrs);  /* If specified, then update and pass on. */
     
     hstatus = mmsbox_url_fetch_content(HTTP_METHOD_POST, mmc->mmsc_url, rh, body, &ph,&rbody);     
     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact MMC[url=%S] => HTTP returned status=[%d]!",
				 mmc->mmsc_url, hstatus);
	  goto done1;
     }
     
     if ((mresp = mm7_parse_soap(ph, rbody)) == NULL) {
	  *error = octstr_format("Failed to parse MMSC[url=%S, id=%S]  response!",
				 mmc->mmsc_url,  mmc->id);
	  goto done1;
     } 

     if (errl) { /* Pick up status stuff -- for DLR */
	  if (*errl == NULL)
	       *errl = http_create_empty_headers();
	  if ((s = mm7_soap_header_value(mresp, octstr_imm("StatusText"))) != NULL) {	  
	       http_header_add(*errl, "X-Mbuni-StatusText", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }

	  if ((s = mm7_soap_header_value(mresp, octstr_imm("Details"))) != NULL) {	  
	       http_header_add(*errl, "X-Mbuni-StatusDetails", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }
     }

     /* Now look at response code and use it to tell you what you want. */
     if ((s = mm7_soap_header_value(mresp, octstr_imm("StatusCode"))) != NULL) {
	  tstatus = atoi(octstr_get_cstr(s));
	  octstr_destroy(s);
     } else if ((s = mm7_soap_header_value(mresp, octstr_imm("faultstring"))) != NULL) {
	  tstatus = atoi(octstr_get_cstr(s));
	  octstr_destroy(s);
     } else
	  tstatus = MM7_SOAP_FORMAT_CORRUPT; 
     

     if (!MM7_SOAP_STATUS_OK(tstatus) && tstatus != MM7_SOAP_COMMAND_REJECTED) {
	  Octstr *detail =  mm7_soap_header_value(mresp, octstr_imm("Details"));
	  char *tmp = (char *)mms_soap_status_to_cstr(tstatus);
	  if (detail == NULL)
	       detail = mm7_soap_header_value(mresp, octstr_imm("faultcode"));
	  ret = NULL;
	  mms_info(0, "MM7", mmc->id, "Send to MMSC[%s], failed, code=[%d=>%s], detail=[%s]", 
	       mmc ? octstr_get_cstr(mmc->id) : "", 
	       tstatus, tmp ? tmp : "", 
	       detail ? octstr_get_cstr(detail) : "");

	  *error = octstr_format("Failed to deliver to MMC[url=%S, id=%S], status=[%d=>%s]!",
				 mmc->mmsc_url, 
				 mmc->id,
				 tstatus, 
				 tmp ? tmp : "");	  
	  octstr_destroy(detail);	  
     } else {	  
	  ret = mm7_soap_header_value(mresp, octstr_imm("MessageID"));	  
	  mms_info(0, "MM7", NULL, "Sent to MMC[%s], code=[%d=>%s], msgid=[%s]", octstr_get_cstr(mmc->id), 
	       tstatus, mms_soap_status_to_cstr(tstatus), ret ? octstr_get_cstr(ret) : "(none)");
     }


     if (ret)
	  mms_log2("Sent", from, to, -1, ret, NULL, mmc->id, "MMSBox", NULL, NULL);
done1:
     *retry = (ret == NULL && (!MM7_SOAP_CLIENT_ERROR(tstatus) || tstatus < 0));
     
     mm7_soap_destroy(mreq);
     mm7_soap_destroy(mresp);	  
     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);
     octstr_destroy(url);

     gwlist_destroy(xto, NULL);

     return ret;
}

static Octstr  *mm7eaif_send(MmscGrp *mmc, Octstr *from, Octstr *to, 
			     Octstr *transid,
			     char *vasid,
			     List *hdrs,
			     MmsMsg *m, Octstr **error,
			     int *retry)
{
     Octstr *ret = NULL, *resp = NULL;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK;
     List *rh = http_create_empty_headers(), *ph = NULL;
     Octstr *body = NULL, *rbody = NULL, *xver = NULL; 
     char *msgtype;

     
     mms_info(0, "MM7", mmc->id,  "MMSBox: Send [eaif] to MMC[%s], msg type [%s], from %s, to %s", 
	  mmc ? octstr_get_cstr(mmc->id) : "", 
	  mms_message_type_to_cstr(mtype), 
	  octstr_get_cstr(from), octstr_get_cstr(to));

     http_header_remove_all(rh, "X-Mms-Allow-Adaptations");	
     http_header_add(rh, "X-NOKIA-MMSC-To", octstr_get_cstr(to));
     http_header_add(rh, "X-NOKIA-MMSC-From", octstr_get_cstr(from));

     xver = octstr_format(EAIF_VERSION, mmc->ver.major, mmc->ver.minor1);
     http_header_add(rh, "X-NOKIA-MMSC-Version", octstr_get_cstr(xver));
     octstr_destroy(xver);

     if (mtype == MMS_MSGTYPE_SEND_REQ || 
	 mtype == MMS_MSGTYPE_RETRIEVE_CONF) {
	  msgtype = "MultiMediaMessage";
	  mms_make_sendreq(m); /* ensure it is a sendreq. */
     } else if (mtype == MMS_MSGTYPE_DELIVERY_IND)
	  msgtype = "DeliveryReport";
     else
	  msgtype = "ReadReply";
     http_header_add(rh, "X-NOKIA-MMSC-Message-Type", msgtype);

     if (hdrs)
	  http_header_combine(rh, hdrs);  /* If specified, then update and pass on. */

     http_header_add(rh, "Content-Type", "application/vnd.wap.mms-message");

     /* Patch the message FROM and TO fields. */
     mms_replace_header_value(m, "From", octstr_get_cstr(from));
     mms_replace_header_value(m, "To", octstr_get_cstr(to));
     mms_replace_header_value(m,"X-Mms-Transaction-ID",
			      transid ? octstr_get_cstr(transid) : "000");
     body = mms_tobinary(m);	       

     hstatus = mmsbox_url_fetch_content(HTTP_METHOD_POST, mmc->mmsc_url, rh, body, &ph, &rbody);

     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact MMC[url=%S] => HTTP returned status = %d !",
				 mmc->mmsc_url, hstatus);
     } else {
	  MmsMsg *mresp = rbody ? mms_frombinary(rbody, octstr_imm("anon@anon")) : NULL;
	  
	  resp = octstr_imm("Ok");
	  if (mresp && mms_messagetype(mresp) == MMS_MSGTYPE_SEND_CONF)
	       resp = mms_get_header_value(mresp, octstr_imm("X-Mms-Response-Status"));	  
	  if (octstr_case_compare(resp, octstr_imm("ok")) != 0)
	       hstatus = HTTP_STATUS_SERVER_ERROR; /* error. */
	  else if (mresp)
	       ret = mms_get_header_value(mresp, octstr_imm("Message-ID"));

	  mms_destroy(mresp);
     }

     if (hstatus < 0)
	  ret = NULL; 
     else {
	  hstatus = http_status_class(hstatus);	  
	  if (hstatus == HTTP_STATUS_SERVER_ERROR ||
	      hstatus == HTTP_STATUS_CLIENT_ERROR) 
	       ret = NULL;
	  else if (!ret) 
	       ret = http_header_value(ph, octstr_imm("X-Nokia-MMSC-Message-Id"));
     }
     *retry = (ret == NULL && (hstatus == HTTP_STATUS_SERVER_ERROR || hstatus < 0));

     if (ret)
	  mms_log2("Sent", from, to, -1, ret, NULL, mmc->id, "MMSBox", NULL, NULL);

#if 0
     mms_info(0, "MM7", mmc->id,"Sent to MMC[%s], code=[%d], resp=[%s] msgid [%s]", 
	  octstr_get_cstr(mmc->id), 
	  hstatus, resp ? octstr_get_cstr(resp) : "(none)", ret ? octstr_get_cstr(ret) : "(none)");
#endif 

     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);

     octstr_destroy(resp);
     return ret;
}


static Octstr  *mm7http_send(MmscGrp *mmc, Octstr *from, Octstr *to,
			     MmsMsg *m, Octstr **error,
			     int *retry)
{
     Octstr *ret = NULL;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK;
     List *rh, *ph = NULL;
     Octstr *body = NULL, *rbody = NULL; 
     Octstr *mms;
     MIMEEntity *form_data = make_multipart_formdata();
     
     mms_info(0, "MM7", mmc->id,  "MMSBox: Send [http] to MMC[%s], msg type [%s], from %s, to %s", 
	  mmc ? octstr_get_cstr(mmc->id) : "", 
	  mms_message_type_to_cstr(mtype), 
	  octstr_get_cstr(from), octstr_get_cstr(to));

     mms = mms_tobinary(m);
     
     add_multipart_form_field(form_data, "to", "text/plain", NULL, to);
     add_multipart_form_field(form_data, "from", "text/plain", NULL, from);
     add_multipart_form_field(form_data, "mms", "application/vnd.wap.mms-message", NULL, mms);

     
     rh = mime_entity_headers(form_data);
     body = mime_entity_body(form_data);

     hstatus = mmsbox_url_fetch_content(HTTP_METHOD_POST, mmc->mmsc_url, rh, body, &ph, &rbody);

     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact MMC[url=%S] => HTTP returned status = %d !",
				 mmc->mmsc_url, hstatus);
     } else {
	  ret = rbody ? octstr_duplicate(rbody) : NULL;
	  if (ret)
	       octstr_strip_blanks(ret);
     }
     
     *retry = (ret == NULL && (http_status_class(hstatus) == HTTP_STATUS_SERVER_ERROR || hstatus < 0));

     if (ret)
	  mms_log2("Sent", from, to, -1, ret, NULL, mmc->id, "MMSBox", NULL, NULL);

     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);
     octstr_destroy(mms);
     mime_entity_destroy(form_data);
     
     return ret;
}


static int mms_sendtommsc(MmscGrp *mmc, MmsEnvelope *e, Octstr *to,
			  Octstr *orig_transid,

			  MmsMsg *m, 
			  Octstr **new_msgid,
			  List **errhdrs) 
{
     Octstr *id = NULL, *groupid = NULL;
     int ret = 0, retry  = 0;
     double throughput = 0;
     Octstr *from = e->from;
     Octstr *transid = e->msgId;
     Octstr *linkedid = e->token; /* token = linkedid */
     char *vasid = e->vasid ? octstr_get_cstr(e->vasid) : NULL;
     Octstr *service_code = e->vaspid;
     Octstr *dlr_url = e->url1;
     Octstr *rr_url = e->url2;
     List *hdrs = e->hdrs;
     
     mutex_lock(mmc->mutex); { /* Grab a lock on it. */
	  Octstr *err = NULL;
	  if (mmc->type == SOAP_MMSC)
	       id = mm7soap_send(mmc, from, to, transid, linkedid, vasid, service_code, hdrs, m, &err, errhdrs, &retry);
	  else if (mmc->type == EAIF_MMSC)
	       id = mm7eaif_send(mmc, from, to, transid, vasid, hdrs, m, &err, &retry);
	  else if (mmc->type == HTTP_MMSC)
	       id = mm7http_send(mmc, from, to, m, &err, &retry);
	  else if (mmc->type == CUSTOM_MMSC && mmc->custom_started) 
	       id = mmc->fns->send_msg(mmc->data, 
				       from, to, transid, linkedid, vasid, 
				       service_code, m, hdrs, &err, &retry);
	  else 
	       mms_error_ex("MT", 0,  "MM7", mmc->id, "MMC[%s] of unknown type, can't send!", 
		     mmc->id ? octstr_get_cstr(mmc->id) : ""); 	       
	
	  throughput = mmc->throughput;
	  groupid = mmc->group_id ? octstr_duplicate(mmc->group_id) : NULL;
	  
	  if (err && errhdrs) {
	       if (*errhdrs == NULL) 
		    *errhdrs = http_create_empty_headers();
	       http_header_add(*errhdrs, "X-Mbuni-Error", octstr_get_cstr(err));
	  }
	  octstr_destroy(err);
     }  mutex_unlock(mmc->mutex);  /* release lock */

     if (id) {
	  if (dlr_url)  /* remember the url's for reporting purposes. */
	       mms_dlr_url_put(id, "delivery-report", groupid, dlr_url, orig_transid);
	  if (rr_url)
	       mms_dlr_url_put(id, "read-report", groupid, rr_url, orig_transid);	  
	  ret = MMS_SEND_OK;
     } else 
	  ret = retry ? MMS_SEND_ERROR_TRANSIENT : MMS_SEND_ERROR_FATAL; 
     
     *new_msgid = id;
          
     octstr_destroy(groupid);
     if (throughput > 0) 
	  gwthread_sleep(1.0/throughput);          
     return ret;
}


static int sendMsg(MmsEnvelope *e)
{
     MmsMsg *msg = NULL;
     int i, n;     
     Octstr *otransid = e->hdrs ? http_header_value(e->hdrs, octstr_imm("X-Mbuni-TransactionID")) : NULL;

     if ((msg = qfs->mms_queue_getdata(e)) == NULL)  {
	  mms_error(0,  "MM7", NULL, "MMSBox queue error: Failed to load message for queue id [%s]!", e->xqfname);
	  goto done2;
     }
     
     for (i = 0, n = gwlist_len(e->to); i<n; i++) {
	  int res = MMS_SEND_OK;
	  MmsEnvelopeTo *to = gwlist_get(e->to, i);
	  Octstr *err = NULL;
	  time_t tnow = time(NULL);
	  MmscGrp *mmc = NULL;
	  Octstr *new_msgid = NULL;
	  List *errl = NULL;
	  int is_email = 0;

	  if (!to || !to->process)
	       continue;
	  
	  if (e->expiryt != 0 &&  /* Handle message expiry. */
	      e->expiryt < tnow) {
               err = octstr_format("MMSC error: Message expired while sending to %S!", to->rcpt);
               res = MMS_SEND_ERROR_FATAL;
	       
               goto done;
          } else if (e->attempts >= maxsendattempts) {
               err = octstr_format("MMSBox error: Failed to deliver to "
				   "%S after %ld attempts. (max attempts allowed is %ld)!", 
                                   to->rcpt, e->attempts, 
                                   maxsendattempts);
               res = MMS_SEND_ERROR_FATAL;
               goto done;
          }

	  is_email = (octstr_search_char(to->rcpt, '@', 0) > 0);
	  
	  if ((mmc = get_handler_mmc(e->viaproxy, to->rcpt, e->from)) == NULL && !is_email) {
               err = octstr_format("MMSBox error: Failed to deliver to "
				   "%S. Don't know how to route!", 
                                   to->rcpt);
               res = MMS_SEND_ERROR_TRANSIENT;
	       goto done;
	  }
	  
	  if (is_email) {
               int j = octstr_case_search(e->from, octstr_imm("/TYPE=PLMN"), 0);
               int k = octstr_case_search(e->from, octstr_imm("/TYPE=IPv"), 0);
               int len = octstr_len(e->from);
               Octstr *pfrom;
	       
               
               if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == len) 
                    pfrom = octstr_copy(e->from, 0, j);
               else if (k > 0 && k + sizeof "/TYPE=IPv" == len) 
                    pfrom = octstr_copy(e->from, 0, k);
               else
                    pfrom = octstr_duplicate(e->from);

               if (octstr_search_char(e->from, '@', 0) < 0)
                    octstr_format_append(pfrom,"@%S", myhostname);
	       
	       res = mms_sendtoemail(pfrom, to->rcpt, 
				     e->subject ? e->subject : octstr_imm(""),
				     e->msgId, msg, 0, &err, octstr_get_cstr(sendmail_cmd),
				     myhostname, 0, 0, 
				     "", 
				     "", 0, 
				     e->xqfname, 
				     e->hdrs);
	       octstr_destroy(pfrom);
	  } else {
	       res = mms_sendtommsc(mmc, e, 
				    to->rcpt, 
				    otransid,
				    msg, 
				    &new_msgid,
				    &errl);
	       if (errl)
		    err = http_header_value(errl, octstr_imm("X-Mbuni-Error"));
	  }
	  if (mmc) {
	       if (res == MMS_SEND_OK)
		    mmc->mt_pdus++;
	       else
		    mmc->mt_errors++;	  
	       mmc->last_pdu = time(NULL);
	       return_mmsc_conn(mmc); /* important. */
	  }
     done:
	  if (res == MMS_SEND_OK || res == MMS_SEND_QUEUED) {
	       to->process = 0;
	       
	       if (e->msgtype == MMS_MSGTYPE_SEND_REQ ||
		   e->msgtype == MMS_MSGTYPE_RETRIEVE_CONF) /* queue dlr as needed. */
		    queue_dlr(mmc, e->from, to->rcpt, new_msgid, octstr_imm("Forwarded"), "MM7-Out", errl);
	  } else if (res == MMS_SEND_ERROR_FATAL && mmc) {
	       if (e->msgtype == MMS_MSGTYPE_SEND_REQ ||
		   e->msgtype == MMS_MSGTYPE_RETRIEVE_CONF) /* queue dlr as needed. */		    
		    queue_dlr(mmc, e->from, to->rcpt, e->msgId, 			
			      (e->expiryt != 0 && e->expiryt < tnow) ? 
			 octstr_imm("Expired") : octstr_imm("Rejected"), 
			      "MM7-Out", errl);
	  }
	  if (res == MMS_SEND_ERROR_FATAL)
	       to->process = 0; /* No more attempts. */        


	  /* handle CDR */
	  if (res == MMS_SEND_OK || res == MMS_SEND_QUEUED || res == MMS_SEND_ERROR_FATAL) {
	       Octstr *mclass = mms_get_header_value(msg, octstr_imm("X-Mms-Message-Class"));
	       Octstr *prio = mms_get_header_value(msg, octstr_imm("X-Mms-Priority"));
	       Octstr *mstatus  = mms_get_header_value(msg, octstr_imm("X-Mms-Status"));
	       
	       /* Do CDR */
	       cdrfs->logcdr(e->created, 
			     octstr_get_cstr(e->from),
			     octstr_get_cstr(to->rcpt),
			     octstr_get_cstr(e->msgId),
			     mmc ? octstr_get_cstr(mmc->id) : NULL, /* Should we touch mmc here? XXX */ 
			     e->src_interface, 
			     "MM7",
			     e->msize, 
			     (char *)mms_message_type_to_cstr(e->msgtype),
			     
			     prio ? octstr_get_cstr(prio) : NULL,
			     mclass ? octstr_get_cstr(mclass) : NULL,
			     res == MMS_SEND_ERROR_FATAL ? "dropped" : (mstatus ? octstr_get_cstr(mstatus) : "sent"),
			     e->dlr,
			     0);
	       
	       octstr_destroy(mclass);
	       octstr_destroy(prio);
	       octstr_destroy(mstatus);	       
	  }

	  if (err == NULL)
	       mms_info(0, "MM7", NULL, "%s MMSBox Outgoing Queue MMS Send: From %s, to %s, msgsize=%ld: msgid=[%s]", 
			SEND_ERROR_STR(res),
			octstr_get_cstr(e->from), octstr_get_cstr(to->rcpt), e->msize,
			new_msgid ? octstr_get_cstr(new_msgid) : "N/A");
	  else 
	       mms_error_ex("MT", 0, 
			    "MM7", NULL, 
			    "%s MMSBox Outgoing Queue MMS Send: From %s, to %s, msgsize=%ld: %s", 
			    SEND_ERROR_STR(res),
			    octstr_get_cstr(e->from), octstr_get_cstr(to->rcpt), e->msize, octstr_get_cstr(err));
	  
	  octstr_destroy(new_msgid);
	  octstr_destroy(err);
	  http_destroy_headers(errl);
	  
	  e->lasttry = tnow;
          if (qfs->mms_queue_update(e) == 1) { 
               e = NULL;
               break; /* Queue entry gone. */             
          }
     }

done2:
     mms_destroy(msg);     
     octstr_destroy(otransid);
     
     if (e) { /* Update the queue if it is still valid (e.g. recipients not handled) 
	       * XXX can this happen here??... 
	       */
	  e->lasttry = time(NULL);
	  e->attempts++;  /* Update count of number of delivery attempts. */   
	  e->sendt = e->lasttry + mmsbox_send_back_off * e->attempts;
	  
	  if (qfs->mms_queue_update(e) != 1)
	       qfs->mms_queue_free_env(e);
     }

     
     return 1; /* always delete queue entry. */
}

void mmsbox_outgoing_queue_runner(volatile sig_atomic_t *rstop)
{
     qfs->mms_queue_run(octstr_get_cstr(outgoing_qdir), 
		   sendMsg, queue_interval, maxthreads, rstop);
}

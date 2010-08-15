/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS client sender: notifications/reports to clients via WAP Push, 
 * manages out-going messages.
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include "mmsc.h"
#include <errno.h>
#include <unistd.h>
#include <strings.h>

#define WAPPUSH_PORT 2948


static MmsEnvelope *update_env(MmsEnvelope *e, MmsEnvelopeTo *xto, int success)
{
     time_t tnow = time(NULL);
     
     if (success && xto &&
	 e->msgtype != MMS_MSGTYPE_SEND_REQ && 
	 e->msgtype != MMS_MSGTYPE_RETRIEVE_CONF)
	  xto->process = 0; /* No more processing, unless it is a SEND/RETRIEVE */
     
     e->lasttry = tnow;
     e->attempts++;     	  
     e->sendt = tnow + settings->send_back_off * e->attempts;	  

     if (settings->qfs->mms_queue_update(e) == 1)
	  e = NULL;    		    
     
     return e;
} 

static void do_mm1_push(Octstr *rcpt_to, int isphonenum, MmsEnvelope *e, MmsMsg *msg)
{
     List *pheaders;
     static unsigned char ct; /* Transaction counter -- do we need it? */
     Octstr *to = NULL;     
     Octstr *pduhdr = octstr_create("");     
     Octstr *s = NULL;     
     
     if (!rcpt_to) {
	  mms_error(0, "MM1", NULL, "mobilesender: Queue entry %s has no recipient address!", e->xqfname);
	  goto done;
     } else
	  to = octstr_duplicate(rcpt_to);
     

     ct++;    
     octstr_append_char(pduhdr, ct);  /* Pushd id */
     octstr_append_char(pduhdr, 0x06); /* PUSH */

     
#if 1
     octstr_append_char(pduhdr, 1 + 1 + 1);
     octstr_append_char(pduhdr, 0xbe); /* content type. */     
#else
     octstr_append_char(pduhdr, 
			1 + 1 + strlen("application/vnd.wap.mms-message") + 1); /*header length. */     
     octstr_append_cstr(pduhdr, "application/vnd.wap.mms-message");
     octstr_append_char(pduhdr, 0x0); /* string terminator. */
#endif
     octstr_append_char(pduhdr, 0xaf); /* push application ID header and value follows. */
     octstr_append_char(pduhdr, 0x84); /* ... */

     s = mms_tobinary(msg);
     if (isphonenum) {
	  Octstr *url = octstr_format("%S&text=%E%E&to=%E&udh=%%06%%05%%04%%0B%%84%%23%%F0",	
				      settings->sendsms_url, pduhdr, s, to);     
	  int status;
	  List *rph  = NULL;
	  Octstr *rbody = NULL;
	  MmsEnvelopeTo *xto = gwlist_get(e->to, 0);
	  
	  pheaders = http_create_empty_headers();
	  http_header_add(pheaders, "Connection", "close");
	  http_header_add(pheaders, "User-Agent", MM_NAME "/" MMSC_VERSION);	       
	  
	  if ((status = mms_url_fetch_content(HTTP_METHOD_GET, url, pheaders, NULL, &rph, &rbody)) < 0 ||
	      http_status_class(status) != HTTP_STATUS_SUCCESSFUL) {
	       
	       mms_error(0,  "MM1", NULL, " Push[%s] from %s, to %s, failed, HTTP code => %d", e->xqfname, 
			 octstr_get_cstr(e->from), octstr_get_cstr(to), status);	       

	       e = update_env(e,xto,0);
	  } else {	 /* Successful push. */      

	       mms_log2("Notify", octstr_imm("system"), to, 
			-1, e ? e->msgId : NULL, NULL, NULL, "MM1", NULL,NULL);
	       e = update_env(e, xto, 1);	       
	  }     
	  
	  http_destroy_headers(pheaders);
	  http_destroy_headers(rph);
	  octstr_destroy(rbody);
	  octstr_destroy(url);
     } else { /* An IP Address: Send packet, forget. */
	  Octstr *addr = udp_create_address(to, WAPPUSH_PORT);
	  int sock = udp_client_socket();
	  MmsEnvelopeTo *xto = gwlist_get(e->to,0);
	  
	  if (sock > 0) {
	       octstr_append(pduhdr, s);
#if 0
	       octstr_dump(pduhdr, 0);
#endif
	       udp_sendto(sock, pduhdr, addr);
	       close(sock); /* ?? */      
	       mms_log2("Notify", octstr_imm("system"), to, 
			-1, e ? e->msgId : NULL, 
			NULL, NULL, "MM1", NULL,NULL);
	       e = update_env(e, xto, 1);
	  } else {
	       e = update_env(e, xto, 0);
	       mms_error(0,  "MM1", NULL, "push to %s:%d failed: %s", 
			 octstr_get_cstr(to), WAPPUSH_PORT, strerror(errno));
	  }
	  octstr_destroy(addr);
     }
 done:
     octstr_destroy(to);
     octstr_destroy(pduhdr);
     octstr_destroy(s);
     
     if (e)
	  settings->qfs->mms_queue_free_env(e);     
}

static int sendNotify(MmsEnvelope *e)
{
     Octstr *to;
     MmsMsg *smsg = NULL;
     MmsEnvelopeTo *xto = gwlist_get(e->to, 0);
     Octstr *err = NULL;
     time_t tnow = time(NULL);
     int j, k, len;
     Octstr *phonenum = NULL, *rcpt_ip = NULL, *msgId, *from, *fromproxy;
     int mtype, msize;
     int res = MMS_SEND_OK, dlr;
     time_t expiryt;
     char *prov_notify_event = NULL;
     char *rtype = NULL;
     
#if 0 /* ... because we don't want fetched messages sticking around in queue forever */           
     
     if (e->lastaccess != 0) { /* This message has been fetched at least once, no more signals. */	  

	  mms_info(0,  "MM1", NULL, "Message [ID: %s] fetched/touched at least once. Skipping",
	       e->xqfname);
	  return settings->qfs->mms_queue_update(e);
     }
#endif

     if (!xto) {
	  mms_error(0,  "MM1", NULL, "mobilesender: Queue entry %s with no recipients!", 
		e->xqfname);
	  return 0;
     }

     to = octstr_duplicate(xto->rcpt);
     expiryt = e->expiryt;
     msgId = e->msgId ? octstr_duplicate(e->msgId) : NULL;
     from = octstr_duplicate(e->from);
     fromproxy = e->fromproxy ? octstr_duplicate(e->fromproxy) : NULL;
     msize = e->msize;
     dlr = e->dlr;
     mtype = e->msgtype;

     if (e->expiryt != 0 &&  /* Handle message expiry. */
	 e->expiryt < tnow) {
	  err = octstr_format("MM1 error: Message expired while sending to %S!", to);
	  res = MMS_SEND_ERROR_FATAL;
	  prov_notify_event = "failedfetch";
	  rtype = "Expired";	  
	  goto done;
     } else if (e->attempts >= settings->mm1_maxsendattempts) {
	  err = octstr_format("MM1: Maximum delivery attempts [%d] to %S reached. Delivery suspended!", 
			      e->attempts, to);
	  res = MMS_SEND_OK;

	  e->sendt = e->expiryt + 1; /* no retry until expiry */	  
	  if (settings->qfs->mms_queue_update(e) != 1) 
	       settings->qfs->mms_queue_free_env(e);		    
	  e = NULL;
	  goto done;

     } else if (e->lastaccess != 0) {
	  e->sendt = e->expiryt + 1;
	  res = MMS_SEND_OK; 
	  err = octstr_create("Skipped");

	  mms_info(0,  "MM1", NULL, "Message [ID: %s] fetched/touched at least once. Skipping",
		   e->xqfname);
	  if (settings->qfs->mms_queue_update(e) != 1) 
	       settings->qfs->mms_queue_free_env(e);
	  e = NULL;

	  goto done;
     }
     

     j = octstr_case_search(to, octstr_imm("/TYPE=PLMN"), 0);
     k = octstr_case_search(to, octstr_imm("/TYPE=IPv"), 0);
     len = octstr_len(to);
     
     if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == len) { /* A proper number. */
	  phonenum = octstr_copy(to, 0, j);
	  mms_normalize_phonenum(&phonenum, 
				 octstr_get_cstr(settings->unified_prefix), 
				 settings->strip_prefixes);	  
     } else if (k > 0 && k + sizeof "/TYPE=IPv" == len) 
	  rcpt_ip = octstr_copy(to, 0, k);
     else {
	  /* We only handle phone numbers here. */
	  err = octstr_format("Unexpected recipient %s in MT queue!", octstr_get_cstr(to));	  
	  res = MMS_SEND_ERROR_FATAL;	  
	  goto done;
     }
     
     
     /* For phone, getting here means the message can be delivered. So: 
      * - Check whether the recipient is provisioned, if not, wait (script called will queue creation req)
      * - If the recipient can't take MMS, then send SMS.
      */
     
     /* We handle two types of requests: send and delivery/read notifications. 
      * other types of messages cannot possibly be in this queue!
      */
     
     if (mtype == MMS_MSGTYPE_SEND_REQ ||
	 mtype == MMS_MSGTYPE_RETRIEVE_CONF) {
	  Octstr *url, *transid;
	  
	  if (phonenum) {
	       int send_ind = mms_ind_send(settings->prov_getstatus, phonenum);
	       
	       if (send_ind < 0 && 
		   settings->notify_unprovisioned)
		    send_ind = 0;

	       if (send_ind < 0) { /* That is, recipient is not (yet) provisioned. */
		    res = MMS_SEND_ERROR_TRANSIENT;
		    err = octstr_format("%S is not provisioned for MMS reception, delivery deferred!", 
					phonenum);
		    
		    /* Do not increase delivery attempts counter. */
		    e->lasttry = tnow;		    
		    e->sendt = e->lasttry + settings->send_back_off * (1 + e->attempts);
		    
		    if (settings->qfs->mms_queue_update(e) == 1) 
			 e = NULL; /* Queue entry gone. */	
		    else 	  
			 settings->qfs->mms_queue_free_env(e);		    		    
		    goto done;
	       } else if (send_ind == 0) { /*  provisioned but does not support */		    
		    Octstr *s = octstr_format(octstr_get_cstr(settings->mms_notify_txt),
					      from);
		    if (settings->notify_unprovisioned && s && octstr_len(s) > 0) { /* Only send if the string was set. */
			 List *pheaders =  http_create_empty_headers(), *rph = NULL;
			 Octstr *rbody = NULL;
			 int status;
			 
			 url = octstr_format("%S&text=%E&to=%E",settings->sendsms_url,s, phonenum);

			 http_header_add(pheaders, "Connection", "close");
			 http_header_add(pheaders, "User-Agent", MM_NAME "/" VERSION);      
			 
			 if ((status = mms_url_fetch_content(HTTP_METHOD_GET, url, 
							     pheaders, NULL, &rph, &rbody)) <0 ||
			     http_status_class(status) != HTTP_STATUS_SUCCESSFUL)
			      mms_error(0, "MM1", NULL, "Notify unprovisioned url fetch failed => %d", status);
			 
			 http_destroy_headers(pheaders);
			 http_destroy_headers(rph);
			 octstr_destroy(url);
			 octstr_destroy(rbody);

		    } 
		    
		    octstr_destroy(s);	
		    res = MMS_SEND_OK;
		    err = octstr_imm("No MMS Ind support, sent SMS instead");
		    
		    xto->process = 0; /* No more processing. */
		    if (settings->qfs->mms_queue_update(e) == 1) 
			 e = NULL; 
		    else 	  
			 settings->qfs->mms_queue_free_env(e);		    
		    goto done;

	       }	       
	  }
	  
	  /* To get here means we can send Ind. */
	  url = mms_makefetchurl(e->xqfname, e->token, MMS_LOC_MQUEUE,
				 phonenum ? phonenum : to,
				 settings);
	  mms_info(0,  "MM1", NULL, "Preparing to notify client to fetch message at URL: %s",
	       octstr_get_cstr(url));
	  transid = mms_maketransid(e->xqfname, settings->host_alias);	  
	  
	  smsg = mms_notification(e->from, e->subject, e->mclass, e->msize, url, transid, 
				  e->expiryt ? e->expiryt :
				  tnow + settings->default_msgexpiry,
				  settings->optimize_notification_size);
	  octstr_destroy(transid);
	  octstr_destroy(url);
     } else if (mtype == MMS_MSGTYPE_DELIVERY_IND ||
		mtype == MMS_MSGTYPE_READ_ORIG_IND) 
	  smsg = settings->qfs->mms_queue_getdata(e);
     else {
	  mms_error(0,  "MM1", NULL, "Unexpected message type [%s] for [%s] found in MT queue!", 
		mms_message_type_to_cstr(mtype), octstr_get_cstr(to));
	  res = MMS_SEND_ERROR_FATAL;
	  goto done;
     }
     
     if (smsg) {
	  do_mm1_push(phonenum ? phonenum : rcpt_ip, 
		      phonenum ? 1 : 0, 
		      e, smsg); /* Don't touch 'e' after this point. It is gone */
	  e = NULL;
     }
     
     if (smsg)
	  mms_destroy(smsg);
    
 done:
     if (e != NULL && 
	 err != NULL && 
	 res != MMS_SEND_ERROR_TRANSIENT && dlr) { /* If there was a report request and this is a legit error
						    *  queue it. 
						    */	 
	       MmsMsg *m = mms_deliveryreport(msgId, to, e->from, tnow, 
					      rtype ? octstr_imm(rtype) : 
					      octstr_imm("Indeterminate"));
	       
	       List *l = gwlist_create();
	       Octstr *res;
	       gwlist_append(l, from);
	       
	       /* Add to queue, switch via proxy to be from proxy. */
	       res = settings->qfs->mms_queue_add(to ? to : settings->system_user, l,  err, 
						  NULL, fromproxy,  
						  tnow, tnow+settings->default_msgexpiry, m, NULL, 
						  NULL, NULL,
						  NULL, NULL,
						  NULL,
						  0,
						  octstr_get_cstr(settings->global_queuedir), 
						  "MM2",
						  settings->host_alias);
	       gwlist_destroy(l, NULL);
	       mms_destroy(m);
	       octstr_destroy(res);	  	  
     }
     
     /* Write to log */
     mms_info(0,  "MM1", NULL, "%s Mobile Queue MMS Send Notify: From=%s, to=%s, msgsize=%d, reason=%s. Processed in %d secs", 
	      SEND_ERROR_STR(res),
	      octstr_get_cstr(from), octstr_get_cstr(to), msize,
	      err ? octstr_get_cstr(err) : "",
	      (int)(time(NULL) - tnow));
     
     
     if (xto && e) {
	  if (res == MMS_SEND_ERROR_FATAL)
	       xto->process = 0;  /* No more attempts to deliver, delete this. */	       
	  if (settings->qfs->mms_queue_update(e) == 1) 
	       e = NULL; /* Queue entry gone. */	
	  else 	  
	       settings->qfs->mms_queue_free_env(e);
     }    /* Else queue will be updated/freed elsewhere. */
     
     
     if (prov_notify_event)
	  notify_prov_server(octstr_get_cstr(settings->prov_notify), 
			     to ? octstr_get_cstr(to) : "unknown", 
			     prov_notify_event, 
			     rtype ? rtype : "",
			     e ? e->msgId : NULL, NULL, NULL);
     
     octstr_destroy(phonenum);
     
     octstr_destroy(rcpt_ip);
     octstr_destroy(to);
     octstr_destroy(msgId);
     octstr_destroy(fromproxy);
     octstr_destroy(from);
     octstr_destroy(err);

     return 1; /* Tell caller we dealt with envelope */
}

void mbuni_mm1_queue_runner(volatile sig_atomic_t  *rstop)
{
     settings->qfs->mms_queue_run(octstr_get_cstr(settings->mm1_queuedir), 
		   sendNotify, settings->mm1_queue_interval, settings->maxthreads, rstop);
     gwthread_sleep(2); /* Wait for it to die. */
     return;     
}



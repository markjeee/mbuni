/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS Global Queue Runner, routes messages generally. 
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <unistd.h>
#include "mmsc.h"

#define NMAX 256
static char mobile_qdir[NMAX];
static char sendmail_cmd[NMAX];


/* Set the queue directory for messages going to mobile. */
static int mms_setmobile_queuedir(char *mqdir);


/* Send command for sending mail. It will be called as <sendmail> <to> with 
 * headers and message on stdin.
 * 
 * The following  % formatting characters are allowed:
 * f - from address
 * t - recipient
 * s - subject
 * m - message id 
 *
 */
static int mms_setsendmail_cmd(char *sendmail);

/* Queue this message for delivery to mobile terminal. */
static int mms_sendtomobile(Octstr *from, Octstr *to, 
			    Octstr *subject, Octstr *fromproxy, 
			    Octstr *msgid, time_t expires, MmsMsg *m, int dlr, Octstr **error);

/* Send this message via an intermediate proxy (MM4 interface). 
 * The caller must modify the MmsMsg sender and recipient address if necessary.
 */
static int mms_sendtoproxy(Octstr *from, Octstr *to, 
			   Octstr *subject, Octstr *proxy, 
			   char *transid,
			   Octstr *msgid, time_t expires, MmsMsg *msg, 
			   int dlr, Octstr *proxy_sendmail_cmd, List *extra_headers, Octstr **error);

static int mms_sendtovasp(MmsVasp *vasp, Octstr *from, Octstr *to, Octstr *msgId, 
			  List *qh,
			  MmsMsg *m, Octstr **error);

static int match_short_codes(Octstr *phonenum, long short_codes[], int num_codes);

#if 0
/* Send errors */
#define MMS_SEND_OK 0
#define MMS_SEND_ERROR_TRANSIENT -1
#define MMS_SEND_ERROR_FATAL -2
#endif

#define NMAX 256
static char qdir[NMAX];

static List *cdr_list; /* List for cdr as used by cdr consumer thread. */

static int  sendMsg(MmsEnvelope *e)
{
     int i, n;
     MmsMsg *msg = NULL;
     time_t tstart = time(NULL);

     if (e->msgtype == MMS_MSGTYPE_SEND_REQ && 
	 !e->bill.billed) { /* Attempt to bill if not already billed */
	  List *l = gwlist_create();
	  double amt;
	  
	  for (i = 0, n = gwlist_len(e->to); i < n; i++) {
	       MmsEnvelopeTo *to = gwlist_get(e->to, i);
	       Octstr *s = octstr_duplicate(to->rcpt);
	       
	       _mms_fixup_address(&s, 
				  octstr_get_cstr(settings->unified_prefix),
				  settings->strip_prefixes, 1);
	       gwlist_append(l, s);
	  }
	   
	  amt = settings->mms_billfuncs->mms_billmsg(e->from, l, 
						     e->msize, 
						     e->vaspid,
						     e->msgId,
						     settings->mms_bill_module_data);
	  gwlist_destroy(l, (void *)octstr_destroy);
	  
	  mms_info(0, "MM2", NULL, "Global Queue MMS Bill: From %s, to_count=%ld, msgid=%s, msgsize=%ld: returned=%.2f", 
	       octstr_get_cstr(e->from), gwlist_len(e->to), e->msgId ? octstr_get_cstr(e->msgId) : "", 
	       e->msize, amt);

	  if (amt == -1) { /* Delete message. */
	       for (i = 0, n = gwlist_len(e->to); i < n; i++) {
		    MmsEnvelopeTo *to = gwlist_get(e->to, i);
		    to->process = 0;
	       }
	  } else if (amt >= 0) {
	       e->bill.billed = 1;
	       e->bill.amt = amt;
	  } 

	  if (amt >= -1)
	       if (settings->qfs->mms_queue_update(e) == 1) /* Write queue just in case we crash. */
		    e = NULL;
	  
	  if (e == NULL ||
	      !e->bill.billed)
	       goto done2; /* If queue is gone, or we didn't manage to bill, go away */
     }

     
     
     msg =  settings->qfs->mms_queue_getdata(e);
#if 0
     if (msg) mms_msgdump(msg,1);
#endif


     for (i = 0, n = gwlist_len(e->to); i < n; i++) {
	  Octstr *err = NULL;
	  int res = MMS_SEND_OK, m;
	  MmsEnvelopeTo *to = gwlist_get(e->to, i);
	  time_t tnow = time(NULL);
	  char *dst_int = "NONE";
	  
	  if (!to || !to->process) /* Already processed. */
	       continue;
	  
	  if (e->expiryt != 0 &&  /* Handle message expiry. */
	      e->expiryt < tnow) {
	       err = octstr_format("MMSC error: Message expired while sending to %S!", to->rcpt);
	       res = MMS_SEND_ERROR_FATAL;

	       goto done;
	  } else if (e->attempts >= settings->maxsendattempts) {
	       err = octstr_format("MMSC error: Failed to deliver to %S after %ld attempts. (max attempts allowed is %ld)!", 
				   to->rcpt, e->attempts, 
				   settings->maxsendattempts);
	       res = MMS_SEND_ERROR_FATAL;
	       goto done;
	  }

	  /* - first check if it is an email address.
	   * - Next check if proxy to send through is already set, use it. 
	   * - else we have a number of IP, try to deliver...
	   */
	  
	  if (octstr_search_char(to->rcpt, '@', 0) > 0) {
	       int j = octstr_case_search(e->from, octstr_imm("/TYPE=PLMN"), 0);
	       int k = octstr_case_search(e->from, octstr_imm("/TYPE=IPv"), 0);
	       int len = octstr_len(e->from);
	       Octstr *pfrom;

	       dst_int = "MM3";
	       
	       if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == len) 
		    pfrom = octstr_copy(e->from, 0, j);
	       else if (k > 0 && k + sizeof "/TYPE=IPv" == len) 
		    pfrom = octstr_copy(e->from, 0, k);
	       else
		    pfrom = octstr_duplicate(e->from);
	       
	       if (octstr_search_char(e->from, '@', 0) < 0)
		    octstr_format_append(pfrom,"@%S", settings->hostname);
	       
	       if (settings->mms2email && /* send, but don't loop back on it */
		   !(strcmp(e->src_interface, "MM7") == 0 &&  
		     e->vaspid && settings->mms2email->id && 
		     octstr_compare(e->vaspid, settings->mms2email->id) == 0))
		    res = mms_sendtovasp(settings->mms2email, 
					 e->from, to->rcpt, 
					 e->msgId,
					 e->hdrs,
					 msg, &err);
	       else
		    res = mms_sendtoemail(pfrom, to->rcpt, 
					  e->subject ? e->subject : settings->mms_email_subject, 
					  e->msgId, msg, 0, &err, sendmail_cmd,
					  settings->hostname, 1, 1, 
					  octstr_get_cstr(settings->mms_email_txt), 
					  octstr_get_cstr(settings->mms_email_html), 0, e->xqfname, 
					  e->hdrs);
	       if (res == MMS_SEND_QUEUED)
		    res = MMS_SEND_OK; /* queued to email treated same as sent. 
					* XXX - this means DLR requests for emailed messages not supported!
					*/
	       if (res == MMS_SEND_OK) 
		    mms_log2("Sent", e->from, to->rcpt, 
			     -1, e->msgId, NULL, NULL, "MM3", NULL,NULL);
	       if (pfrom != e->from)
		    octstr_destroy(pfrom);
	  } else {
	       int j = octstr_case_search(to->rcpt, octstr_imm("/TYPE=PLMN"), 0);
	       int k = octstr_case_search(to->rcpt, octstr_imm("/TYPE=IPv"), 0);
	       int len = octstr_len(to->rcpt);
	       Octstr *phonenum = NULL;
	       Octstr *mmsc;
	       int sent = 0;
	       MmsVasp *vasp;
	       List *vlist;
	       
	       /* If it is an IP, send to mobile handler, else if number, look for recipient. */
	       if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == len) 
		    phonenum = octstr_copy(to->rcpt, 0, j);
	       else if (k > 0 && k + sizeof "/TYPE=IPv" == len) {
		    if (settings->mms2mobile && e->msgtype == MMS_MSGTYPE_SEND_REQ) { /* Send a copy to this VASP. */
			 Octstr *xerr = NULL;
			 int res = mms_sendtovasp(settings->mms2mobile, 
						  e->from, to->rcpt, 
						  e->msgId,
						  e->hdrs,
						  msg, &err);
			 mms_info(0, "MM7", settings->mms2mobile->id, "%s Global Queue MMS Send: Local Msg copy to VASP (%s) - "
			      "From %s, to %s, msgsize=%ld: err=%s", 
			      SEND_ERROR_STR(res),
			      octstr_get_cstr(settings->mms2mobile->id),
			      octstr_get_cstr(e->from), octstr_get_cstr(to->rcpt), e->msize,
			      xerr ? octstr_get_cstr(xerr) : "(null)");			 
			 if (xerr)
			      octstr_destroy(xerr);
		    }
		    res = mms_sendtomobile(e->from, 
					   to->rcpt, e->subject, e->fromproxy, 
					   e->msgId, e->expiryt, msg, e->dlr, &err);    
		    dst_int = "MM1";
		    sent = 1;
		    goto done;
	       } else {
		    /* We don't handle other types for now. */
		    err = octstr_format("MMSC error: Unsupported recipient type %S", to->rcpt);
		    res = MMS_SEND_ERROR_FATAL;
		    
		    goto done;
	       }
	       
	       /* Search VASP list, see what you can find... */
	       vlist = dict_keys(settings->vasp_list);
	       for (j = 0, m = gwlist_len(vlist); j < m; j++)
		    if ((vasp = dict_get(settings->vasp_list, gwlist_get(vlist, j))) != NULL &&
			match_short_codes(phonenum, vasp->short_codes, vasp->num_short_codes)) {
			 res = mms_sendtovasp(vasp, e->from, to->rcpt,
					      e->msgId,
					      e->hdrs,
					      msg, &err);
			 dst_int = "MM7";
			 sent = 1;
			 break;
		    }
	       gwlist_destroy(vlist, (void *)octstr_destroy);

	       if (sent != 1) { /* Not yet, sent, find the receiver MMSC. */
		    /* Normalise the number, then see if we can resolve home MMSC for this recipient. */
#if 0
		    normalize_number(octstr_get_cstr(settings->unified_prefix), &phonenum);
#else
		    mms_normalize_phonenum(&phonenum, 
					   octstr_get_cstr(settings->unified_prefix), 
					   settings->strip_prefixes);
#endif	    
		    if ((mmsc = settings->mms_resolvefuncs->mms_resolve(phonenum, 
									e->src_interface, 
									e->vaspid ? octstr_get_cstr(e->vaspid) : NULL,
									settings->mms_resolver_module_data,
									settings, proxyrelays))) {
			 mms_info(0, "MM2", NULL, "mmsc for \"%s\" resolved to: \"%s\"", 
			      octstr_get_cstr(phonenum), octstr_get_cstr(mmsc));
			 /* If resolved to my hostname, then this is local. */
			 if (octstr_compare(mmsc, settings->hostname) == 0) {
			      if (settings->mms2mobile && 
				  e->msgtype == MMS_MSGTYPE_SEND_REQ) { /* Send a copy to this VASP. */
				   Octstr *xerr = NULL;
				   int res = mms_sendtovasp(settings->mms2mobile, 
							    e->from, to->rcpt, 
							    e->msgId,
							    e->hdrs,
							    msg, &err);
				   mms_info(0, "MM2", NULL, "%s Global Queue MMS Send: Local Msg copy to VASP (%s) - "
					"From %s, to %s, msgsize=%ld: err=%s", 
					SEND_ERROR_STR(res),
					octstr_get_cstr(settings->mms2mobile->id),
					octstr_get_cstr(e->from), octstr_get_cstr(to->rcpt), e->msize,
					xerr ? octstr_get_cstr(xerr) : "(null)");			 
				   if (xerr)
					octstr_destroy(xerr);
			      }
			      
			      res =  mms_sendtomobile(e->from, to->rcpt, 
						      e->subject, e->fromproxy, 
						      e->msgId, e->expiryt, msg, e->dlr,
						      &err);
			      dst_int = "MM1";
			      sent = 1;
			 } else {

			   /* else,  step through proxies, look for one that matches. */    
			      for (j = 0, m = proxyrelays ? gwlist_len(proxyrelays) : 0; j<m; j++) {
				   MmsProxyRelay *mp = gwlist_get(proxyrelays, j);
				   
				   if (octstr_compare(mp->host, mmsc) == 0) {
					Octstr *xtransid = mms_maketransid(e->xqfname, 
									   settings->host_alias);
					Octstr *xfrom = e->from ? octstr_duplicate(e->from) : octstr_create("anon@anon");
					Octstr *xto   = octstr_duplicate(to->rcpt);
					
					_mms_fixup_address(&xfrom, 
							   mp->unified_prefix ? octstr_get_cstr(mp->unified_prefix) : NULL,
							   mp->strip_prefixes, 1);
					
					_mms_fixup_address(&xto, 
							   mp->unified_prefix ? octstr_get_cstr(mp->unified_prefix) : NULL,
							   mp->strip_prefixes, 1);					
					res = mms_sendtoproxy(xfrom, xto, 
							      e->subject, mp->host,
							      octstr_get_cstr(xtransid),			      
							      e->msgId, e->expiryt, msg,
							      mp->confirmed_mm4, mp->sendmail, e->hdrs, &err);
					dst_int = "MM4";
					sent = 1;
					octstr_destroy(xtransid);
					octstr_destroy(xfrom);
					octstr_destroy(xto);
					break;
				   }
			      }
			      
			      if (sent != 1) { /* try mm7 delivery. Again. */
				   List *vlist = dict_keys(settings->vasp_list);
				   Octstr *vid;
				   for (j = 0, m = gwlist_len(vlist); j < m; j++)
					if ((vid = gwlist_get(vlist, j)) != NULL && 
					    (vasp = dict_get(settings->vasp_list, vid)) != NULL &&
					    vasp->id && 
					    octstr_compare(vasp->id, mmsc) == 0) {
					     res = mms_sendtovasp(vasp, e->from, to->rcpt,
								  e->msgId,
								  e->hdrs,
								  msg, &err);
					     dst_int = "MM7";
					     sent = 1;
					     break;
					}
				   
				   gwlist_destroy(vlist, (void *)octstr_destroy);
			      }
			 }
			 octstr_destroy(mmsc);
		    }
	       }
	       if (!sent) {
		    res = MMS_SEND_ERROR_FATAL;
		    err = octstr_format("MMSC error: Don't know how to deliver to %S !", to->rcpt);
	       }
	       octstr_destroy(phonenum);
	  }

     done:
	  if (res == MMS_SEND_OK)
	       to->process = 0;
	  else { /* If there was a report request, queue it. */
	       
	       if (e->dlr) {
		    Octstr *qfs; 
		    char *mstatus;
		    MmsMsg *m;
		    List *l;

		    if (res == MMS_SEND_QUEUED)
			 mstatus = "Forwarded";
		    else if (e->expiryt != 0 && e->expiryt < tnow)
			 mstatus = "Expired";
		    else 
			 mstatus = "Rejected";
		    m = mms_deliveryreport(e->msgId, to->rcpt, e->from, tnow, octstr_imm(mstatus));
		    
		    l = gwlist_create();
		    
		    gwlist_append(l, octstr_duplicate(e->from));
		    
		    /* Add to queue, switch via proxy to be from proxy. */
		    qfs = settings->qfs->mms_queue_add(to->rcpt, l,
						       err, NULL, e->fromproxy,  
						       tnow, tnow+settings->default_msgexpiry, m, NULL, 
						       NULL, NULL,
						       NULL, NULL,
						       NULL,
						       0, 
						       qdir, 
						       "MM2",
						       settings->host_alias);
		    octstr_destroy(qfs);
		    gwlist_destroy(l, NULL);

		    mms_destroy(m);
	       }
	       if (res == MMS_SEND_ERROR_FATAL)
		    to->process = 0; /* No more attempts. */	       
	  }
	  
	  /* Write to log */
	  if (res == MMS_SEND_ERROR_FATAL || 
	      (res == MMS_SEND_ERROR_TRANSIENT && err != NULL))
	       mms_error(0, "MM2", NULL, "%s Global Queue MMS Send [%.128s]: From %s, to %s, msgsize=%ld: %s. ", 
			 SEND_ERROR_STR(res),
			 e->xqfname,
			 octstr_get_cstr(e->from), octstr_get_cstr(to->rcpt), e->msize,
			 err ? octstr_get_cstr(err) : "n/a");
	  else 
	       mms_info(0, "MM2", NULL, "%s Global Queue MMS Send [%.128s]: From %s, to %s, msgsize=%ld: %s.", 
			 SEND_ERROR_STR(res),
			 e->xqfname,
			 octstr_get_cstr(e->from), octstr_get_cstr(to->rcpt), e->msize,
			 err ? octstr_get_cstr(err) : "n/a");
	       
	  if (res == MMS_SEND_OK && 
	      (e->msgtype == MMS_MSGTYPE_SEND_REQ || 
	       e->msgtype == MMS_MSGTYPE_RETRIEVE_CONF)) { /* Do CDR writing. */
	       MmsCdrStruct *cdr = make_cdr_struct(settings->mms_bill_module_data,
						   e->created,
						   octstr_get_cstr(e->from), 
						   octstr_get_cstr(to->rcpt), 
						   e->msgId ? octstr_get_cstr(e->msgId) : "", 
						   e->vaspid ? octstr_get_cstr(e->vaspid) : "",
						   e->src_interface, 
						   dst_int,
						   e->msize);
	       
	       gwlist_produce(cdr_list, cdr);	   /* Put it on list so sending thread sends it. */    	       
	  }
	  octstr_destroy(err);
	  /* Update queue entry so that we know which ones have been processed. */
	  e->lasttry = tnow;
	  if (settings->qfs->mms_queue_update(e) == 1) { 
	       e = NULL;
	       break; /* Queue entry gone. */	  	  
	  }
     }    
     
 done2:     
     
     mms_destroy(msg);     

     if (e) { /* Update the queue if it is still valid (e.g. recipients not handled) XXX can this happen here??... */
	  e->lasttry = time(NULL);
	  e->attempts++;  /* Update count of number of delivery attempts. */   
	  e->sendt = e->lasttry + settings->send_back_off * e->attempts;

	  if (settings->qfs->mms_queue_update(e) != 1)
	       settings->qfs->mms_queue_free_env(e);
     }

     mms_info(0, "MMSC.global", NULL, "Processed in %d secs", (int)(time(NULL) - tstart)); /* report processing time. */

     return 1; /* Always deletes the queue entry. */
}


static void cdr_thread(void *unused)
{
     MmsCdrStruct *cdr;

     while ((cdr = gwlist_consume(cdr_list)) != NULL) {
	  settings->mms_billfuncs->mms_logcdr(cdr);
	  /* We should probably write to log here... */
	  gw_free(cdr);
     }
}

void mbuni_global_queue_runner(volatile sig_atomic_t *rstop) 
{
     long cdr_thid;
     mms_setmobile_queuedir(octstr_get_cstr(settings->mm1_queuedir));

     mms_setsendmail_cmd(octstr_get_cstr(settings->sendmail));

     strncpy(qdir, octstr_get_cstr(settings->global_queuedir), sizeof qdir);


     /* Start the thread for CDR */
     cdr_list = gwlist_create();
     gwlist_add_producer(cdr_list);
     cdr_thid = gwthread_create(cdr_thread, NULL);
   
     
     settings->qfs->mms_queue_run(qdir, sendMsg, settings->queue_interval, settings->maxthreads, rstop);
     /* When it ends, wait a little for other stuff to stop... */
     gwlist_remove_producer(cdr_list); /* Stop CDR thread. */

     if (cdr_thid >= 0) 
	  gwthread_join(cdr_thid);
     gwthread_sleep(2);     
     gwlist_destroy(cdr_list, NULL); 
     
     return;     
}



static int mms_setmobile_queuedir(char *mqdir)
{
     strncpy(mobile_qdir, mqdir, sizeof mobile_qdir);
     return 0;
}


static int mms_setsendmail_cmd(char *sendmail)
{
     strncpy(sendmail_cmd, sendmail, -1 + sizeof sendmail_cmd);
     return 0;
}

/* 
 * Queue this message for delivery to mobile terminal. 
 * A queue thread will handle sending of notifications to phone.
 * When a deliver is received, another thread will remove the queue entry.
 */
int mms_sendtomobile(Octstr *from, Octstr *to, 
			    Octstr *subject, Octstr *fromproxy, 
			    Octstr *msgid, time_t expires, MmsMsg *m, int dlr, Octstr **error)
{

     Octstr *ret = NULL, *x;
     List *l = gwlist_create();
     char tokenstr[128];
     
     gwlist_append(l, to);
     
     /* We generate a special token that will be added to message ID to make 
      * stealing messages a bit harder. 
      */
     snprintf(tokenstr, -1 + sizeof tokenstr, 
	      "wx%ld", 
	      random() % 100);

     x = octstr_create(tokenstr);
     if (m)
	  ret = settings->qfs->mms_queue_add(from, l, subject, fromproxy, NULL, 0, expires, m,
					     x, NULL, NULL, 
					     NULL, NULL,
					     NULL,
					     dlr, mobile_qdir, 
					     "MM2",
					     settings->host_alias);
     else 
	  *error = octstr_format("GlobalSend: Failed to send to %S, Message format is corrupt!", to);

     octstr_destroy(x);

     gwlist_destroy(l, NULL);
     octstr_destroy(ret);
     if (ret == NULL)
	  return (m) ? MMS_SEND_ERROR_TRANSIENT : MMS_SEND_ERROR_FATAL;
     else
	  return MMS_SEND_OK;
}


/* Send this message via an intermediate proxy (MM4 interface). 
 * The way it works: We email the message to the proxy but we also keep a local copy in the queue
 * so we can handle delivery receipts and such.
 */
static int mms_sendtoproxy(Octstr *from, Octstr *to, 
			   Octstr *subject, Octstr *proxy, 
			   char *transid,
			   Octstr *msgid, time_t expires, MmsMsg *msg, 
			   int dlr, Octstr *proxy_sendmail_cmd, List *extra_headers, Octstr **error)
{
  
     Octstr *pto, *pfrom;     
     int x = MMS_SEND_ERROR_FATAL;
     Octstr *xtransid; /* We make a fake transaction ID that includes 'to' field. */
     
     if (!to || 
	 octstr_search_char(to, '@', 0) >= 0) {
	  *error = octstr_format("Bad recipient address sent to  MM4 interface, addresss  is %S!", to);
	  return MMS_SEND_ERROR_FATAL;
     }
     
     if (!msg) {
	  *error = octstr_format("GlobalSend: Failed to send to %S, Message format is corrupt!", to);
	  return MMS_SEND_ERROR_FATAL;
     }

     pto = octstr_format("%S@%S", to, proxy);
     if (octstr_search_char(from, '@', 0) < 0)
	  pfrom = octstr_format("%S@%S", from, settings->hostname);
     else 
	  pfrom = from;
     
     xtransid = octstr_format("%S-%s", to,transid);
     x = mms_sendtoemail(from, pto, 
			 subject ? subject : settings->mms_email_subject, 
			 msgid,  msg, dlr, 
			 error, proxy_sendmail_cmd ? octstr_get_cstr(proxy_sendmail_cmd) : sendmail_cmd,
			 settings->hostname, 0, 0,NULL,NULL,1, octstr_get_cstr(xtransid), extra_headers);
     octstr_destroy(xtransid);

     if (x == MMS_SEND_QUEUED && !dlr) /* No confirmed sending, and message was queued successfully... */
	  x = MMS_SEND_OK; /* we assume fully sent! */
     
     if (x >= 0) /* no error. */
	  mms_log2(x == MMS_SEND_OK ? "Sent" : "Queued", from, pto, 
		   -1, msgid, NULL, proxy, "MM4", NULL,NULL);          
     octstr_destroy(pto);
     if (pfrom != from) 
	  octstr_destroy(pfrom);	 
     return x;
}

static int  _x_octstr_int_compare(int n, Octstr *s)
{
     char x[64];
     
     sprintf(x, "%d", n);     
     return octstr_str_compare(s,x);     
}

static int mm7soap_send(MmsVasp *vasp, Octstr *from, Octstr *to, Octstr *msgId, 
			List *qh,
			MmsMsg *m, Octstr **error, int *got_conn_error)
{
     int ret = MMS_SEND_ERROR_TRANSIENT;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK, tstatus;
     List *xto = gwlist_create();
     MSoapMsg_t *mreq = NULL, *mresp = NULL;
     List *rh = NULL, *ph = NULL;
     Octstr *body = NULL, *rbody = NULL; 
     Octstr *uaprof = NULL, *s;
     time_t tstamp;
     
     mms_info(0, "MM7", vasp->id, "MMS Relay: Send[soap] to VASP[%s], msg_type=[%s], from=[%s], to=[%s]", 
	  vasp ? octstr_get_cstr(vasp->id) : "", 
	  mms_message_type_to_cstr(mtype), octstr_get_cstr(from), octstr_get_cstr(to));    
     
     gwlist_append(xto, to);
     
     if (vasp->send_uaprof == UAProf_URL)
	  uaprof = http_header_value(qh, octstr_imm("X-Mbuni-Profile-Url"));
     else if (vasp->send_uaprof == UAProf_UA)
	  uaprof = http_header_value(qh, octstr_imm("X-Mbuni-User-Agent"));
     
     if ((s = http_header_value(qh, octstr_imm("X-Mbuni-Timestamp"))) != NULL) {
	  tstamp = strtoul(octstr_get_cstr(s), NULL, 10);
	  octstr_destroy(s);
     } else 
	  tstamp = 0;
     
     if ((mreq = mm7_mmsmsg_to_soap(m, from, xto, msgId, settings->host_alias, 
				    msgId,
				    0, NULL, NULL, uaprof, tstamp, NULL)) == NULL) {
	  *error = octstr_format("Failed to convert Msg[%s] 2 SOAP message!",
				 mms_message_type_to_cstr(mtype));
	  goto done1;
     }
     
     if (mm7_soapmsg_to_httpmsg(mreq, &vasp->ver, &rh, &body) < 0) {
	  *error = octstr_format("Failed to convert SOAP message 2 HTTP Msg!");
	  goto done1;
     } 
 
     hstatus = mms_url_fetch_content(HTTP_METHOD_POST, vasp->vasp_url, rh, body, &ph, &rbody);
     *got_conn_error = (hstatus < 0);
     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact VASP[url=%s] => HTTP returned status = %d!",
				 octstr_get_cstr(vasp->vasp_url), hstatus);
	  goto done1;
     }
     
     if ((mresp = mm7_parse_soap(ph, rbody)) == NULL) {
	  *error = octstr_format("Failed to parse VASP[url=%s, id=%s]  response!",
				 octstr_get_cstr(vasp->vasp_url), 
				 octstr_get_cstr(vasp->id));
	  goto done1;
     } 
     
     /* Now look at response code and use it to tell you what you want. */
     if ((s = mm7_soap_header_value(mresp, octstr_imm("StatusCode"))) != NULL) {
	  tstatus = atoi(octstr_get_cstr(s));
	  octstr_destroy(s);
     } else 
	  tstatus = MM7_SOAP_FORMAT_CORRUPT; 
     
     if (!MM7_SOAP_STATUS_OK(tstatus)) {
	  Octstr *detail =  mm7_soap_header_value(mresp, octstr_imm("Details"));
	  ret = MMS_SEND_ERROR_FATAL;
	  mms_info(0, "MM7", vasp->id, "Send to VASP[%s], failed, code=[%d=>%s], detail=%s", 
	       vasp ? octstr_get_cstr(vasp->id) : "", 
	       tstatus, mms_soap_status_to_cstr(tstatus), 
	       detail ? octstr_get_cstr(detail) : "");
	  *error = octstr_format("Failed to deliver to VASP[url=%s, id=%s], status=[%d=>%s]!",
				 octstr_get_cstr(vasp->vasp_url), 
				 octstr_get_cstr(vasp->id),
				 tstatus, mms_soap_status_to_cstr(tstatus));
	  
	  if (detail)
	       octstr_destroy(detail);
	  
     } else
	  ret = MMS_SEND_OK;
     
     mms_info(0, "MM7", vasp->id, "Sent to VASP[%s], code=[%d=>%s]", octstr_get_cstr(vasp->id), 
	  tstatus, mms_soap_status_to_cstr(tstatus));
 done1:
     

     mm7_soap_destroy(mreq);     
     mm7_soap_destroy(mresp);
     http_destroy_headers(rh);
     
     octstr_destroy(body);
     http_destroy_headers(ph);
     
     octstr_destroy(rbody);
     octstr_destroy(uaprof);
     gwlist_destroy(xto, NULL);

     return ret;
}

static int mm7eaif_send(MmsVasp *vasp, Octstr *from, Octstr *to, Octstr *msgid, 
			MmsMsg *m, Octstr **error, int *got_conn_error)
{
     int ret = MMS_SEND_ERROR_TRANSIENT;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK;
     List *rh = http_create_empty_headers(), *ph = NULL;
     Octstr *body = NULL, *rbody = NULL, *url = NULL, *xver; 
     char *msgtype;

     
     mms_info(0, "MM7", vasp->id, "MMS Relay: Send [eaif] to VASP[%s], msg_type=[%s], from=[%s], to=[%s]", 
	  vasp ? octstr_get_cstr(vasp->id) : "", 
	  mms_message_type_to_cstr(mtype), octstr_get_cstr(from), octstr_get_cstr(to));
     
     http_header_add(rh, "X-NOKIA-MMSC-To", octstr_get_cstr(to));
     http_header_add(rh, "X-NOKIA-MMSC-From", octstr_get_cstr(from));
     if (msgid) 
	  http_header_add(rh, "X-NOKIA-MMSC-Message-Id", octstr_get_cstr(msgid));
     
     xver = octstr_format(EAIF_VERSION, vasp->ver.major, vasp->ver.minor1);
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
     http_header_add(rh, "Content-Type", "application/vnd.wap.mms-message");

     body = mms_tobinary(m);	       
     
     hstatus = mms_url_fetch_content(HTTP_METHOD_POST, vasp->vasp_url, rh, body, &ph, &rbody);
     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact VASP[url=%s] => HTTP returned status = %d!",
				 octstr_get_cstr(vasp->vasp_url), hstatus);
     } else 	  	 	  	      
	  mms_info(0, "MM7", vasp->id, "Sent to VASP[%s], code=[%d]", octstr_get_cstr(vasp->id), hstatus);

     if (hstatus < 0) {
	  ret = MMS_SEND_ERROR_TRANSIENT; 
	  *got_conn_error = (hstatus < 0);
     } else {
	  hstatus = http_status_class(hstatus);	  
	  if (hstatus == HTTP_STATUS_CLIENT_ERROR)
	       ret = MMS_SEND_ERROR_TRANSIENT;
	  else if (hstatus == HTTP_STATUS_SERVER_ERROR)
	       ret = MMS_SEND_ERROR_FATAL;
	  else 
	       ret = MMS_SEND_OK;
     }

     
     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);
     octstr_destroy(url);
     
     return ret;
}

static int mms_sendtovasp(MmsVasp *vasp, Octstr *from, Octstr *to, Octstr *msgid, 
			  List *qh,
			  MmsMsg *m, Octstr **err) 
{
     int ret, conn_err = 0;
     if (m == NULL) {
	  *err = octstr_format("GlobalSend: Failed to send to %S, Message format is corrupt!", to);
	  ret = MMS_SEND_ERROR_FATAL;
     } else if (vasp->type == SOAP_VASP)
	  ret = mm7soap_send(vasp, from, to, msgid, qh, m, err, &conn_err);
     else if (vasp->type == EAIF_VASP)
	  ret = mm7eaif_send(vasp, from, to, msgid, m, err, &conn_err);
     else {
	  mms_error(0, "MM7", vasp->id, "Vasp[%s] of unknown type, can't send!", 
		vasp->id ? octstr_get_cstr(vasp->id) : ""); 
	  ret = MMS_SEND_ERROR_FATAL;	  
     }

     if (ret < 0) /* failed. */
	  vasp->stats.mt_errors++;
     
     if (!conn_err) {
	  vasp->stats.mt_pdus++;
	  vasp->stats.last_pdu = time(NULL);
     }
     return ret;
}

static int match_short_codes(Octstr *phonenum, long short_codes[], int num_codes)
{
     int i;

     for (i = 0; i<num_codes;i++)
	  if (_x_octstr_int_compare(short_codes[i], phonenum) == 0)
	       return 1;
     return 0;
}

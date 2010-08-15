/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS Proxy interface, implements HTTP interface for client transactions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "mms_msg.h"
#include "mms_queue.h"

#include "mms_uaprof.h"
#include "mmsc_cfg.h"
#include "mms_mm7soap.h"
#include "mmsc.h"

#define MAX_MESSAGE_SIZE 100*1024

typedef struct MmsHTTPClientInfo {
     HTTPClient *client;
     Octstr *ua;
     Octstr *ip;
     List   *headers;
     Octstr *url;
     Octstr *body;
     List   *cgivars;
     Octstr *profile_url;
     Octstr *base_client_addr;
     Octstr *client_addr;
     MmsVasp *vasp;
} MmsHTTPClientInfo;

static long mm7_thread = -1;
static volatile sig_atomic_t rstop = 0;

static void free_clientInfo(MmsHTTPClientInfo *h, int freeh);
static void fetchmms_proxy(MmsHTTPClientInfo *h);
static void sendmms_proxy(MmsHTTPClientInfo *h);

static void mm7proxy(void *unused);

static void mm1proxy(void)
{
     
     MmsHTTPClientInfo h = {NULL};
     
     
     while(rstop == 0 && (h.client = http_accept_request(settings->port, 
							 &h.ip, &h.url, &h.headers, 
							 &h.body, &h.cgivars)) != NULL) 
	  if (is_allowed_ip(settings->allow_ip, settings->deny_ip, h.ip)) {
	       MmsHTTPClientInfo *hx = gw_malloc(sizeof *hx);
	       
	       h.vasp = NULL;
	       h.profile_url = NULL;	       
	       h.ua = http_header_value(h.headers, octstr_imm("User-Agent"));
	       
	       /* Get the profile URL and store it. Has effect of fetching if missing. */
	       if ((h.profile_url = http_header_value(h.headers, 
						      octstr_imm("X-Wap-Profile"))) == NULL)
		    h.profile_url = http_header_value(h.headers, octstr_imm("Profile"));	  
	       
	       if (h.profile_url) {
		    /*  If quoted, get first quoted string */
		    if (octstr_get_char(h.profile_url, 0) == '"') {
			 int x;
			 octstr_delete(h.profile_url, 0, 1);
			 x = octstr_search_char(h.profile_url, '"', 0);
			 if (x >= 0)
			      octstr_delete(h.profile_url, x, octstr_len(h.profile_url));
		    }
		    octstr_strip_blanks(h.profile_url);
	       }

	       /* Get the sender address. */
	       h.base_client_addr = mms_find_sender_msisdn(h.url, 
							   h.ip,
							   h.headers, 
							   settings->wap_gw_msisdn_header,
							   settings->wap_gw_ip_header,
							   settings->mms_detokenizefuncs);  

	       if (!h.base_client_addr) { /* Set to IP sender... XXXX assumes ipv4 only for now*/
		    if (settings->allow_ip_type) {
			int ipv6 = 0;
			h.base_client_addr = mms_find_sender_ip(h.headers,
								settings->wap_gw_ip_header, 
								h.ip, &ipv6);
			h.client_addr  = octstr_format("%S/TYPE=IPv%s", 
						       h.base_client_addr, 
						       ipv6 ? "6" : "4");
		    } else 
		         h.client_addr = NULL;
		    
	       } else if (octstr_search_char(h.base_client_addr, '.', 0) >= 0 || 
			  octstr_search_char(h.base_client_addr, ':', 0) >= 0) { /* We got back an IP address above, so normalise. */
		    if (octstr_case_search(h.base_client_addr, octstr_imm("TYPE="),0) < 0)
			 h.client_addr = octstr_format("%S/TYPE=IPv%s", h.base_client_addr, 
						       octstr_search_char(h.base_client_addr, ':', 0) >= 0 ? "6" : "4");
		    else 
			 h.client_addr = octstr_duplicate(h.base_client_addr);
	       } else  { /* A bare number, normalise it. */
		    _mms_fixup_address(&h.base_client_addr, 
				       octstr_get_cstr(settings->unified_prefix), 
				       settings->strip_prefixes, 0);		    
		    h.client_addr = octstr_format("%S/TYPE=PLMN", h.base_client_addr);	
	       }
	       
	       debug("mmsproxy", 0, 
		     " Request, ip=%s, base_client_addr=%s, client_addr=%s, url=%s ", 
		     h.ip ? octstr_get_cstr(h.ip) : "", 
		     h.base_client_addr ? octstr_get_cstr(h.base_client_addr) : "",
		     h.client_addr ? octstr_get_cstr(h.client_addr) : "", 
		     octstr_get_cstr(h.url));     

	       /* Dump headers, url etc. */
#if 0
	       http_header_dump(h.headers);
	       if (h.body) octstr_dump(h.body, 0);
	       if (h.ip) octstr_dump(h.ip, 0);
#endif
	       /* Determine if this is the fetch interface or the send interface being used, 
		* by checking if http request has a body.
		* then call relevant function in a thread (use threads because these functions 
		* can block)
		*/
	       *hx = h; /* Copy it all over. */
	       if (hx->body == NULL || octstr_len(hx->body) == 0)
		    gwthread_create((gwthread_func_t *)fetchmms_proxy, hx);
	       else 
		    gwthread_create((gwthread_func_t *)sendmms_proxy, hx);		   
	       	       
	  } else {	  
	       
	       http_close_client(h.client);

	       free_clientInfo(&h, 0);
	  }

     mms_info(0, "MM1", NULL, "Mmsproxy [mm1]: Shutdown commenced...");
     
}

int mmsproxy(void)
{
     
     if (!(settings->svc_list & (SvcMM7 | SvcMM1)))  {
	  mms_info(0, "mmsproxy", NULL, " " MM_NAME " MMSC Proxy version %s, no services to be started.", MMSC_VERSION);
	  return 0;
     } else 
	  mms_info(0, "mmsproxy", NULL, " " MM_NAME " MMSC Proxy version %s starting", MMSC_VERSION);
     
     mms_start_profile_engine(octstr_get_cstr(settings->ua_profile_cache_dir));
     
     if (settings->svc_list & SvcMM7) {
	  /* If we have mm7 port, start thread for it. */
	  
	  if (settings->mm7port > 0 && 
	      http_open_port(settings->mm7port, 0) >= 0) 
	       mm7_thread = gwthread_create((gwthread_func_t *)mm7proxy, NULL);
	  else
	       mms_warning(0, "MM7", NULL,"MMS Proxy: MM7 interface not open, port=%ld",
			   settings->mm7port);
     } else 
	  mm7_thread = -1;
     
     if (settings->svc_list & SvcMM1) {
	  /* Now open port and start dispatching requests. */
	  if (http_open_port(settings->port, 0) < 0) 
	       mms_error(0, "MM1", NULL, "MMS Proxy: Failed to start http server: %d => %s!", 
			 errno, strerror(errno));
	  else 
	       mm1proxy(); /* run mm1 proxy in current thread. */
     }
     
     if (mm7_thread >0) 
	  gwthread_join(mm7_thread);
     
     mms_info(0, "mmsproxy", NULL, "Stopping profile engine...");
     mms_stop_profile_engine(); /* Stop profile stuff. */
     
     mms_info(0,  "mmsproxy", NULL,"Shutdown complete.");
     return 0;
}


void stop_mmsproxy(void)
{
     mms_info(0,  "mmsproxy", NULL, "Shutdown commenced...");
     rstop = 1;

     if (settings->svc_list & SvcMM1) 
	  http_close_port(settings->port);
     if (settings->svc_list & SvcMM7) 
	  http_close_port(settings->mm7port);
     if (mm7_thread >= 0)
	  gwthread_wakeup(mm7_thread);
     mms_info(0,  "mmsproxy", NULL, "Signalling shutdown complete.");
}

void fetchmms_proxy(MmsHTTPClientInfo *h)
{
     Octstr *dlr_flag = NULL;
     Octstr *qf  = NULL, *token = NULL, *s = NULL, *transid = NULL;
     MmsEnvelope *e = NULL;
     MmsMsg *m = NULL, *mr = NULL;
     List *rh;
     char *notify_cmd = NULL, *notify_arg = NULL;
     int loc, menc = MS_1_1;
     MmsUaProfile *prof = NULL;

     debug("proxy.fetchinterface", 0, " ---> Entered fetch interface: url=%s <---", 
	   h->url ? octstr_get_cstr(h->url) : "none");


     /* handle mm5 */
     if (settings->mm5)
	  settings->mm5->update(octstr_get_cstr(h->base_client_addr), 
				h->profile_url ? octstr_get_cstr(h->profile_url) : NULL,
				h->ua ? octstr_get_cstr(h->ua) : NULL);

     rh = http_create_empty_headers();
     http_header_add(rh, "Pragma", "no-cache");
     http_header_add(rh, "Cache-Control", "no-cache");

     if (mms_decodefetchurl(h->url, &qf, &token,&loc) != 0) {
	  mms_error(0, "MM1", NULL, "MMS Fetch interface: failed to decode request url (%s) from %s!", 
		octstr_get_cstr(h->url), 
		octstr_get_cstr(h->ip));
	  goto failed;
     }

     if (h->profile_url) {
	  prof = mms_get_ua_profile(octstr_get_cstr(h->profile_url));
	  if (!prof)
	       prof = mms_make_ua_profile(h->headers);
     } else
	  prof = mms_make_ua_profile(h->headers);

     if (loc == MMS_LOC_MQUEUE) { /* where is the message? */
	  e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf), 
				     octstr_get_cstr(settings->mm1_queuedir), 1);
	  
	  if (!e ||
	      (m = settings->qfs->mms_queue_getdata(e)) == NULL) {
	       mms_error(0, "MM1", NULL,"MMS Fetch interface: failed to find envelope/data %s for request url (%s) from %s (e=%s)!", 
		     octstr_get_cstr(qf), octstr_get_cstr(h->url), octstr_get_cstr(h->ip),
		     (e)? "found" : "not found");  
	       
	       mr = mms_retrieveconf(NULL, NULL, "Error-permanent-message-not-found", "Message not found", 
				     settings->system_user, MS_1_1);
	       s = mms_tobinary(mr);
	       notify_cmd = "fetchfailed";
	       notify_arg = "messagenotfound";
	       goto failed;   
	  }
     } else { /* it is in mmbox, so get it from there. */
	  unsigned long msize;
	  if ((m = mms_mmbox_get(octstr_get_cstr(settings->mmbox_rootdir),
				 octstr_get_cstr(h->client_addr), qf, &msize)) == NULL) {
	       mms_error(0, "MM1", NULL,"MMS Fetch interface: failed to find data in MMBOX %s, url=%s, from=%s (e=%s)!", 
		     octstr_get_cstr(qf), octstr_get_cstr(h->url), octstr_get_cstr(h->ip),
		     (e)? "found" : "not found");  	       
	       mr = mms_retrieveconf(NULL, NULL, "Error-permanent-message-not-found", "Message not found", 
				     settings->system_user, MS_1_1);
	       s = mms_tobinary(mr);
	       notify_cmd = "fetchfailed";
	       notify_arg = "messagenotfound";
	       goto failed;   
	  }  
	  menc = MS_1_2;
     }
     /* Adapt content, if turned on. */
     transid = mms_maketransid(octstr_get_cstr(qf), settings->host_alias);
     dlr_flag = mms_get_header_value(m, octstr_imm("X-Mms-Delivery-Report"));
     if (settings->content_adaptation) {
	  MmsMsg *outmsg = NULL;
	  int x = mms_transform_msg(m, prof, &outmsg);
	  
	  if (x == -1) { /* Temporary failure, we need to fetch profile. */
	       mr = mms_retrieveconf(NULL, transid, "Error-transient-failure", 
				     "Mobile MMS Settings not found",
				     settings->system_user,MS_1_1);
	       s = mms_tobinary(mr);
	       goto failed;   	       
	  } else if (x < 0) { /* Doesn't support MMS */
#if 0
	       mr = mms_retrieveconf(NULL, transid, 
				     "Error-permanent-content-unsupported", 
				     "No MMS Support",
				     settings->system_user,MS_1_1);
	       s = mms_tobinary(mr);
	       notify_cmd = "fetchfailed";
	       notify_arg = "device-does-not-support-mms";
	       goto failed;   	       
#else
	       /* Just accept the message. */
#endif
	  } else if (x == 0) {
	       if (outmsg == NULL) { /* Too large so truncated. */
		    Octstr *xx = octstr_format(octstr_get_cstr(settings->mms_toolarge), 
					       (e) ? e->from : h->client_addr);
		    
		    mr = mms_retrieveconf(NULL, transid, 
#if 0
					  "Error-permanent-content-unsupported", 
#else
					  "Ok",
#endif
					  octstr_get_cstr(xx), 
					  settings->system_user,MS_1_1);
		    octstr_destroy(xx);
		    
                    s = mms_tobinary(mr);
                    notify_cmd = "fetchfailed";
                    notify_arg = "message-too-large-for-device";
                    goto failed;
	       } else {
		    mms_destroy(m);
		    m = outmsg;
	       }		
	  }	       
     }
     
     mr = mms_retrieveconf(m, transid, "Error-permanent-message-not-found", NULL, 
			   (e) ? e->from : h->client_addr, menc);
     s = mms_tobinary(mr);
     
     if (!m) {
	  mms_error(0, "MM1", NULL,"MMS Fetch interface: Failed to get message, url=%s, loc=%d from %s!", 
		octstr_get_cstr(h->url), loc, octstr_get_cstr(h->ip));  
	  goto failed;   	  
     }
     
     if (!s) {
	  mms_error(0, "MM1", NULL,"MMS Fetch interface: Failed to convert message to binary for "
		"request url (%s) from %s!", 
		octstr_get_cstr(h->url), octstr_get_cstr(h->ip));  
	  goto failed;   	  
     }
     
     notify_cmd = "fetched";

     if (settings->dlr_on_fetch && 
	 dlr_flag && octstr_str_case_compare(dlr_flag, "Yes") == 0 && 
	  e != NULL) {
	  char tbuf[64];
	  Octstr *x, *from = h->client_addr ? h->client_addr : settings->system_user;
	  List *l = gwlist_create(), *qh = gwlist_create();	  
	  MmsMsg *mrpt = mms_deliveryreport(e->msgId, from, e->from, 
					    time(NULL), octstr_imm("Retrieved"));

	  gwlist_append(l, octstr_duplicate(e->from));
	  
	  /* Record user agent and profile url. */
	  if (h->ua)
	       http_header_add(qh, "X-Mbuni-User-Agent", octstr_get_cstr(h->ua));
	  if (h->profile_url)
	       http_header_add(qh, "X-Mbuni-Profile-Url", octstr_get_cstr(h->profile_url));
	  sprintf(tbuf, "%ld", time(NULL));
	  http_header_add(qh, "X-Mbuni-Timestamp", tbuf); /* record time of message. */

	  x = settings->qfs->mms_queue_add(from, l,  NULL, NULL, NULL, 0, 
					   time(NULL) + settings->default_msgexpiry, mrpt, NULL, 
					   NULL, NULL,
					   NULL, NULL,
					   qh,
					   0,
					   octstr_get_cstr(settings->global_queuedir), 
					   "MM1",
					   settings->host_alias);
	  
	  octstr_destroy(x);
	  
	  gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
	  http_destroy_headers(qh);
	  mms_destroy(mrpt);	 	  
     }

     if (e) {
	  e->lastaccess = time(NULL); /* No more notifications requests. */
	  e->sendt = e->expiryt + 3600*24*7; /* keep it for a week. */
	  settings->qfs->mms_queue_update(e);
     }

     http_header_add(rh, "Content-Type", "application/vnd.wap.mms-message");
     http_send_reply(h->client, HTTP_OK, rh, s);

#if 1
     debug("proxy.fetchinterface", 0, 
	   " $$$$$$ fetch message replying with [type=%s,content_len=%ld]: ", 
 	   mr ? mms_message_type_to_cstr(mms_messagetype(mr)) : (unsigned char *)"none", 
	   s ? octstr_len(s) : 0);     
     if (mr)
	  mms_msgdump(mr,1);
#endif

     /* Send to access log with success. */
     mms_log2("Fetched", e ? e->from : NULL, h->client_addr, 
	     e ? e->msize : 0, e ? e->msgId : NULL, NULL, NULL, "MM1", 
	      h->ua, (loc == MMS_LOC_MMBOX) ? qf : NULL);     
     goto free_stuff; /* Skip to end. */
     
 failed:          
#if 1
     debug("proxy.fetchinterface", 0, 
	   " $$$$$$ fetch message [fail] replying with [type=%s,content_len=%ld]: ", 
 	   mr ? mms_message_type_to_cstr(mms_messagetype(mr)) : (unsigned char *)"none", 
	   s ? octstr_len(s) : 0);          
     if (mr)
	  mms_msgdump(mr,1);
#endif
     
     /* Send to access log on failure?? */
     mms_log2("Failed Fetch", e ? e->from : NULL, h->client_addr, 
	     e ? e->msize : 0, e ? e->msgId : NULL, NULL, NULL, "MM1", h->ua, 
	      (loc == MMS_LOC_MMBOX) ? qf : NULL);

     if (!s) {
	  http_header_add(rh, "Content-Type", "text/plain");
	  http_send_reply(h->client, HTTP_NOT_FOUND, rh, octstr_imm("Not found"));
     } else {
	  http_header_add(rh, "Content-Type", "application/vnd.wap.mms-message");
	  http_send_reply(h->client, HTTP_OK, rh, s);
     }

 free_stuff:
     if (notify_cmd) /* Inform provisioning server */	  
	  notify_prov_server(octstr_get_cstr(settings->prov_notify), 
			     h->base_client_addr ? octstr_get_cstr(h->base_client_addr) : "unknown", 
			     notify_cmd, notify_arg ? notify_arg : "", e ? e->msgId : NULL,
			     h->ua, h->profile_url);

     
     http_destroy_headers(rh);
     settings->qfs->mms_queue_free_env(e);    

     octstr_destroy(s);
     mms_destroy(m);     
     mms_destroy(mr);     

     octstr_destroy(qf);
     octstr_destroy(token);
     octstr_destroy(transid);
     octstr_destroy(dlr_flag);
     free_clientInfo(h,1);
}

/* Make list of recipients and also sender. */

static void sendmms_proxy(MmsHTTPClientInfo *h)
{
     List *rh = http_create_empty_headers();
     MmsMsg *m, *mresp = NULL;
     Octstr *reply_body = NULL;
     int ctype_set = 0;
     int mtype = 0, menc;
     int hstatus = HTTP_OK;
     char *notify_cmd = NULL, tbuf[64];
     int msize = h->body ? octstr_len(h->body) : 0;
     List *qh = http_create_empty_headers();
     char *xmtype;
     
     debug("proxy.sendinterface", 0, 
	   " --> Enterred sendmms interface, blen=%d <--- ", 
	   msize);
     
     /* handle mm5 */
     if (settings->mm5)
	  settings->mm5->update(octstr_get_cstr(h->base_client_addr), 
				h->profile_url ? octstr_get_cstr(h->profile_url) : NULL,
				h->ua ? octstr_get_cstr(h->ua) : NULL);

     http_header_add(rh, "Pragma", "no-cache");
     http_header_add(rh, "Cache-Control", "no-cache");

     if (!h->body) { /* A body is required. */
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;

	  xmtype = (void *)mms_message_type_to_cstr(mtype);
	  reply_body = octstr_format("Unexpected MMS message[%d: %s], no body?", 
				     mtype, xmtype ? xmtype : "(null)");

	  goto done;
     }      

     m = mms_frombinary_ex(h->body, 
			   h->client_addr ? h->client_addr : octstr_imm("anon@anon"),
			   octstr_get_cstr(settings->unified_prefix), 
			   settings->strip_prefixes);

     if (!m) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  ctype_set = 1;
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Malformed MMS message");
	  debug("proxy.sendinterface", 0, " Parse error on incoming message.");
	  goto done;
     }

     /* Record user agent and profile url. */
     if (h->ua)
	  http_header_add(qh, "X-Mbuni-User-Agent", octstr_get_cstr(h->ua));
     if (h->profile_url)
	  http_header_add(qh, "X-Mbuni-Profile-Url", octstr_get_cstr(h->profile_url));
     sprintf(tbuf, "%ld", time(NULL));
     http_header_add(qh, "X-Mbuni-Timestamp", tbuf); /* record time of message. */

     debug("proxy.sendinterface", 0, " Client sent us: ");
     
#if 0
     mms_msgdump(m,1);
     /* octstr_dump(h->body, 0); */
#endif
     mtype = mms_messagetype(m);
     menc = mms_message_enc(m);     
     
     switch(mtype) {
	  
     case MMS_MSGTYPE_SEND_REQ:
     {
	  Octstr *qf;
	  List *mh = mms_message_headers(m);
	  Octstr *from = h->client_addr ? octstr_duplicate(h->client_addr) : NULL;
	  List *to = gwlist_create();
	  Octstr *subject = NULL;
	  time_t expiryt, deliveryt;
	  Octstr *otransid = NULL, *value = NULL, *msgid = NULL;
	  int dlr;
	  char *mmbox_store_status = NULL;
	  Octstr *mmbox_loc = NULL;
	  Octstr *sdf = NULL;
	  
	  mms_collect_envdata_from_msgheaders(mh, &to, &subject, &otransid, &expiryt, 
					      &deliveryt, settings->default_msgexpiry,
					      settings->max_msgexpiry,
					      NULL, NULL); /* already normalized. */
	  
	  if (!h->client_addr) {
	       mresp = mms_sendconf("Error-sending-address-unresolved", "None", 
				    octstr_get_cstr(otransid),0,
				    menc);
	       mms_error(0, "MM1", NULL,"MMS Send interface: failed to find sender address in request from %s!", 
		     octstr_get_cstr(h->ip));	       

	  } else {
	       Octstr *x = mms_get_header_value(m, octstr_imm("X-Mms-Store"));
		    
	       mms_remove_headers(m, "X-Mms-Store");

	       
	       if (menc >= MS_1_2 && 
		   x != NULL &&
		   octstr_case_compare(x, octstr_imm("Yes")) == 0) {

		    sdf = mms_mmbox_addmsg(octstr_get_cstr(settings->mmbox_rootdir),
					   octstr_get_cstr(h->client_addr), m, 
					   NULL,
					   octstr_imm("Sent"));
		    
		    /* XXX perhaps qualify errors better? */
		    mmbox_store_status = sdf ? "Success" : "Error-permanent-failure";
		    if (sdf) 
			 mmbox_loc = mms_makefetchurl(octstr_get_cstr(sdf), NULL, MMS_LOC_MMBOX, from, settings); 
	       }
	       

	       octstr_destroy(x);

	      /*Delete some headers that must be sent on. */
	      mms_remove_headers(m, "Bcc");
	      mms_remove_headers(m, "X-Mms-Delivery-Time");
	      mms_remove_headers(m, "X-Mms-Expiry");
	      mms_remove_headers(m, "X-Mms-Sender-Visibility");

	      mms_remove_headers(m, "X-Mms-MM-Flags");	      
	      mms_remove_headers(m, "X-Mms-MM-State");	      


	      value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"));	  
	      if (value && 
		  octstr_case_compare(value, octstr_imm("Yes")) == 0) 
		   dlr = 1;
	      else 
		   dlr = 0;	      
	      qf = settings->qfs->mms_queue_add(from, to, subject, 
						NULL, NULL, deliveryt, expiryt, m, NULL, 
						NULL, NULL,
						NULL, NULL,
						qh,
						dlr,
						octstr_get_cstr(settings->global_queuedir), 
						"MM1",
						settings->host_alias);
	      
	      if (!qf) 
		   mresp = mms_sendconf("Error-transient-failure", "None", octstr_get_cstr(otransid),0,
					menc);
	      else {
		   msgid = mms_make_msgid(octstr_get_cstr(qf), 
					   settings->host_alias);		  
		   mresp = mms_sendconf("Ok", octstr_get_cstr(msgid), octstr_get_cstr(otransid),0,
					menc);

		   /* Log to access log */
		   mms_log("Received", from, to, msize, msgid, NULL, NULL, "MM1", h->ua, sdf);

		   octstr_destroy(qf);
	      }
	      
	      if (mmbox_store_status)  /* If saved to mmbox, ... */
		   mms_replace_header_value(mresp, 
					    "X-Mms-Store-Status", mmbox_store_status);
	      if (mmbox_loc) 
		   mms_replace_header_value(mresp, 
					    "X-Mms-Content-Location", 
					    octstr_get_cstr(mmbox_loc));
	  }	  
	  octstr_destroy(otransid);
	  octstr_destroy(value);
	  octstr_destroy(msgid);
	  octstr_destroy(mmbox_loc);
	  octstr_destroy(sdf);
	  octstr_destroy(from);
	  octstr_destroy(subject);
	  http_destroy_headers(mh);

	  gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);

	  notify_cmd = "sent";
	  reply_body = mms_tobinary(mresp);
     }	  
     break;
     case MMS_MSGTYPE_FORWARD_REQ:
     {
	  Octstr *qf = NULL, *token = NULL;
	  List *mh = mms_message_headers(m);
	  Octstr *from = h->client_addr ? octstr_duplicate(h->client_addr) : NULL;
	  List *to = gwlist_create();
	  Octstr  *subject;
	  time_t expiryt, deliveryt;
	  MmsMsg *mfwd = NULL;
	  MmsEnvelope *e = NULL;
	  int mloc;

	  Octstr *otransid;
	  Octstr *url = http_header_value(mh, octstr_imm("X-Mms-Content-Location"));
	  Octstr *read_report = http_header_value(mh, octstr_imm("X-Mms-Read-Report"));
	  Octstr *delivery_report = http_header_value(mh, 
						      octstr_imm("X-Mms-Delivery-Report"));
	  Octstr *allow_report = http_header_value(mh, 
						   octstr_imm("X-Mms-Report-Allowed"));
	  int dlr;
	  unsigned long msize;	  
	  char *mmbox_store_status = NULL;
	  Octstr *mmbox_loc = NULL;
	  Octstr *sdf = NULL;


	  mms_collect_envdata_from_msgheaders(mh, &to, &subject, &otransid, &expiryt, 
					      &deliveryt, settings->default_msgexpiry,
					      settings->max_msgexpiry,
					      NULL, NULL);
	  
	  if (!h->client_addr) {
	       mresp = mms_sendconf("Error-sending-address-unresolved", "None", octstr_get_cstr(otransid),1,
				    menc);
	       mms_error(0, "MM1", NULL,"MMS Send interface (fwd): failed to find sender address in request from %s!", 
		     octstr_get_cstr(h->ip));	       
	       goto forward_done;
	  }

	  if (url == NULL || 
	      mms_decodefetchurl(url, &qf, &token,&mloc) != 0) {
	       mms_error(0, "MM1", NULL,"MMS Send interface: failed to decode forward url (%s) from %s!", 
		     url ? octstr_get_cstr(url) : "(null)", 
		     octstr_get_cstr(h->ip));
	       mresp = mms_sendconf("Error-permanent-message-not-found", "None", 
				    octstr_get_cstr(otransid),1,menc);	    
	       goto forward_done;
	  }

	  if (mloc == MMS_LOC_MQUEUE) { /* where is the message? */
	       e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf), 
					  octstr_get_cstr(settings->mm1_queuedir), 1);
	       
	       if (!e ||
		   (mfwd = settings->qfs->mms_queue_getdata(e)) == NULL) {
		    mms_error(0, "MM1", NULL,
			  "MMS Send interface: failed to find envelope/data %s for forward url "
			  "(%s) from %s (e=%s)!", 
			  octstr_get_cstr(qf), octstr_get_cstr(url), octstr_get_cstr(h->ip),
			  (e) ? "found" : "not found");  
		    
		    mresp = mms_sendconf("Error-permanent-message-not-found", "None", 
					 octstr_get_cstr(otransid),1,menc);	    
		    goto forward_done;
	       }      
	  } else   /* it is in mmbox, so get it from there. */
	       if ((mfwd = mms_mmbox_get(octstr_get_cstr(settings->mmbox_rootdir),
				      octstr_get_cstr(h->client_addr), qf, &msize)) == NULL) {
		    mms_error(0, "MM1", NULL,"MMS Send interface: failed to find data in MMBOX %s, "
			  "forward_url=%s, from=%s (e=%s)!", 
			  octstr_get_cstr(qf), octstr_get_cstr(h->url), octstr_get_cstr(h->ip),
			  (e)? "found" : "not found");  
		    mresp = mms_sendconf("Error-permanent-message-not-found", "None", 
					 octstr_get_cstr(otransid),1,menc);	    
		    
		    goto forward_done;   
	       }  
	  
	  { /* Found it, etc. */
	       Octstr *pfrom = mms_get_header_value(mfwd, octstr_imm("From"));		    
	       Octstr *pdate = mms_get_header_value(mfwd, octstr_imm("Date"));
	       Octstr *pmsgid = mms_get_header_value(mfwd, octstr_imm("Message-ID"));
	       Octstr *pdelivery_report = mms_get_header_value(mfwd, 
							       octstr_imm("X-Mms-Delivery-Report"));
	       Octstr *msgid = NULL;
	       Octstr *s;
	       Octstr *qf2;
	       int n = 0;
	       
	       Octstr *xstate = mms_get_header_value(m, octstr_imm("X-Mms-MM-State"));
	       List *xflags = mms_get_header_values(m,  octstr_imm("X-Mms-MM-Flags"));
	       
	       Octstr *x = NULL;


	       /* Modify the message before sending on as per spec. */
	       mms_replace_header_value(mfwd, "From", octstr_get_cstr(from));
	       
	       mms_remove_headers(mfwd, "X-Mms-Read-Report");
	       if (read_report)
		    mms_replace_header_value(mfwd, "X-Mms-Read-Report", 
					     octstr_get_cstr(read_report));
	       
	       mms_remove_headers(mfwd, "X-Mms-Delivery-Report");
	       if (delivery_report)
		    mms_replace_header_value(mfwd, "X-Mms-Delivery-Report", 
					     octstr_get_cstr(delivery_report));
	       
	       
	       if ((s = mms_get_header_value(mfwd, octstr_imm("X-Mms-Previously-Sent-By"))) != NULL) {
		    sscanf(octstr_get_cstr(s), "%d", &n);
		    octstr_destroy(s);
	       }
	       s = octstr_format("%d%S", n+1, pfrom);			 
	       mms_replace_header_value(mfwd, "X-Mms-Previously-Sent-By",  octstr_get_cstr(s));
	       
	       
	       if ((s = mms_get_header_value(mfwd, 
					     octstr_imm("X-Mms-Previously-Sent-Date"))) 
		   != NULL) {
		    sscanf(octstr_get_cstr(s), "%d", &n);
		    octstr_destroy(s);
	       }
	       if (pdate) {
		    s = octstr_format("%d%S", n+1, pdate);			 
		    mms_replace_header_value(mfwd, 
					     "X-Mms-Previously-Sent-Date",  
					     octstr_get_cstr(s));
		    octstr_destroy(pdate);
	       }
	       if (delivery_report &&
		   octstr_case_compare(delivery_report, octstr_imm("Yes")) == 0)
		    dlr = 1;
	       else 
		    dlr = 0;
	       /* Message to forward is now ready, write it to queue. */
	       qf2 = settings->qfs->mms_queue_add(from, to, subject, 
						  NULL, NULL, deliveryt, expiryt, mfwd, NULL,
						  NULL, NULL,
						  NULL, NULL,
						  qh,
						  dlr,
						  octstr_get_cstr(settings->global_queuedir), 
						  "MM1",
						  settings->host_alias);
	  
	       /* Process any requests for writing to mmbox. */
	       x = mms_get_header_value(m, octstr_imm("X-Mms-Store"));
	       mms_remove_headers(m, "X-Mms-Store");	      
	       
	       if (menc >= MS_1_2 && 
		   x != NULL &&
		   octstr_case_compare(x, octstr_imm("Yes")) == 0) {
		    if (mloc != MMS_LOC_MMBOX) { /* not in mmbox, add it. */
			 sdf = mms_mmbox_addmsg(octstr_get_cstr(settings->mmbox_rootdir),
						octstr_get_cstr(h->client_addr), mfwd, 
						xflags,
						xstate ? xstate : octstr_imm("Forwarded"));
			 
			 /* XXX perhaps qualify errors better? */
			 mmbox_store_status = sdf ? "Success" : "Error-permanent-failure";
			 if (sdf) 
			      mmbox_loc = mms_makefetchurl(octstr_get_cstr(sdf), NULL, 
							   MMS_LOC_MMBOX, from, settings); 
		    } else { /* otherwise simply mod it. */
			 int xret;
			 
			 xret = mms_mmbox_modmsg(octstr_get_cstr(settings->mmbox_rootdir),
						 octstr_get_cstr(h->client_addr), qf, 
						 xstate ? xstate : octstr_imm("Forwarded"),
						 xflags);
			 
			 /* XXX perhaps qualify errors better? */
			 mmbox_store_status = (xret == 0) ? "Success" : "Error-permanent-failure";
			 if (xret == 0)
			      mmbox_loc = mms_makefetchurl(octstr_get_cstr(qf), NULL, 
							   MMS_LOC_MMBOX, from, settings); 

		    }
	       }
	       
	       octstr_destroy(x);	       
	       if (!qf2) 
		    mresp = mms_sendconf("Error-transient-failure", 
					 "None", octstr_get_cstr(otransid),1,menc);
	       else {
		    msgid = mms_make_msgid(octstr_get_cstr(qf2), settings->host_alias);
		    mresp = mms_sendconf("Ok", 
					 octstr_get_cstr(msgid), 
					 octstr_get_cstr(otransid),1,menc);
		    
		    /* Log to access log */
		    mms_log("Forwarded", h->client_addr, to, msize, msgid, NULL, NULL, "MM1", 
			    h->ua, sdf);
		    
		    octstr_destroy(qf2);
	       }

	       if (mmbox_store_status)  /* If saved to mmbox, ... */
		    mms_replace_header_value(mresp, 
					     "X-Mms-Store-Status", mmbox_store_status);
	       if (mmbox_loc) 
		    mms_replace_header_value(mresp, 
					     "X-Mms-Content-Location", 
					     octstr_get_cstr(mmbox_loc));

	       
	       /* You have queued it, now check if the original sender asked for a delivery notify. 
		* if so and this forward has not refused it, then send a notify and we are done for now. 
		*/
	       
	       if ((!allow_report ||
		    octstr_case_compare(allow_report, octstr_imm("Yes")) == 0) &&
		   (pdelivery_report && octstr_case_compare(pdelivery_report, 
							    octstr_imm("Yes")))) {
		    Octstr *from = h->client_addr ? h->client_addr : settings->system_user;
		    MmsMsg *mrep = mms_deliveryreport(pmsgid, from, e ? e->from : NULL, time(NULL), 
						      octstr_imm("Forwarded"));
		    Octstr *x;
		    List *l = gwlist_create();
		    gwlist_append(l, pfrom);
		    
		    x = settings->qfs->mms_queue_add(from, l, NULL, NULL, NULL, 0, 
						     time(NULL) + settings->default_msgexpiry, 
						     mrep, NULL,
						     NULL, NULL,
						     NULL, NULL,
						     NULL,
						     0,				      
						     octstr_get_cstr(settings->global_queuedir), 
						     "MM1",
						     settings->host_alias);
		    octstr_destroy(x);
		    
		    gwlist_destroy(l, NULL);
 		    mms_destroy(mrep);
	       }

	       octstr_destroy(pfrom);
	       octstr_destroy(pdelivery_report);
	       octstr_destroy(pmsgid);
	       octstr_destroy(msgid);	       
	       octstr_destroy(xstate);
	       gwlist_destroy(xflags, (gwlist_item_destructor_t *)octstr_destroy);
	  }  
	  
     forward_done:
	  
	  mms_destroy(mfwd);
	  
	  if (e) { /* Update the message queue and go. */
	       e->lastaccess = time(NULL);
	       if (settings->qfs->mms_queue_update(e) != 1) /* Should be freed. */	       
		    settings->qfs->mms_queue_free_env(e);
	       e = NULL;
	  } 
	  
	  octstr_destroy(qf);
	  octstr_destroy(token);
	  octstr_destroy(from);
	  octstr_destroy(subject);
	  http_destroy_headers(mh);
	  gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
	  octstr_destroy(otransid);
	  octstr_destroy(url);
	  octstr_destroy(read_report);
	  octstr_destroy(allow_report);
	  octstr_destroy(delivery_report);
	  octstr_destroy(mmbox_loc);
	  octstr_destroy(sdf);

	  reply_body = mms_tobinary(mresp);
	  notify_cmd = "fetched";	  
     }
     break;
     case MMS_MSGTYPE_NOTIFYRESP:
     case MMS_MSGTYPE_ACKNOWLEDGE_IND: 
     {
	  Octstr *transid = mms_get_header_value(m, octstr_imm("X-Mms-Transaction-ID"));
	  Octstr *allow_report = mms_get_header_value(m, octstr_imm("X-Mms-Report-Allowed"));
	  
	  Octstr *qf = mms_getqf_fromtransid(transid);
	  MmsEnvelope *e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf),
						  octstr_get_cstr(settings->mm1_queuedir), 1);
	  Octstr *status;
	  
	  MmsMsg *mrpt;
	  
	  if (m)	    
	       mms_msgdump(m,1);

	  if (mtype == MMS_MSGTYPE_NOTIFYRESP)
	       status  = mms_get_header_value(m, octstr_imm("X-Mms-Status"));
	  else  /* This (acknowledge-ind) is the same as notify_resp with status=retrieved. */
	       status = octstr_imm("Retrieved");
	  
	  if (!e) {
	       mms_error(0, "MM1", NULL,"MMS Send interface: Received notification type=%s "
		     "[url=%s, transid=%s, qf=%s] but could not find queue entry!\n", 
		     mms_message_type_to_cstr(mtype), octstr_get_cstr(h->url), 
		     octstr_get_cstr(transid), octstr_get_cstr(qf));  	       
	       goto mdone;
	  }
	  
	  if (octstr_str_compare(status, "Retrieved") == 0) {
	       MmsEnvelopeTo *t = gwlist_get(e->to, 0);		    
	       if (t) 
		    t->process = 0;		    
	  } else
	       e->lastaccess = time(NULL); /* Note now that it has been touched. */
	  
	  /* If the allow report header is missing (default is Yes) 
	   * or it is there and has said we must send report, 
	   * and sender requested a report, then queue a report. 
	   */
	  if ((allow_report == NULL 
	      || octstr_case_compare(allow_report, octstr_imm("Yes")) == 0) &&
	      e->dlr) {		    
	       Octstr *x;
	       Octstr *from = h->client_addr ? h->client_addr : settings->system_user; 
	       List *l = gwlist_create();

	       mrpt = mms_deliveryreport(e->msgId, from, e->from, time(NULL), status);
	       gwlist_append(l, octstr_duplicate(e->from));
	       
	       x = settings->qfs->mms_queue_add(from, l,  NULL, NULL, NULL, 0, 
						time(NULL) + settings->default_msgexpiry, mrpt, NULL, 
						NULL, NULL,
						NULL, NULL,
						qh,
						0,
						octstr_get_cstr(settings->global_queuedir), 
						"MM1",
						settings->host_alias);
	       octstr_destroy(x);

	       gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
	       mms_destroy(mrpt);
	  }	       
	  
     mdone:

	  /* Log to access log */
	  mms_log2("NotifyResp", h->client_addr, NULL, msize, transid, status, NULL, "MM1", 
		   h->ua, NULL);

	  if (e && 
	      settings->qfs->mms_queue_update(e) != 1) /* Should be freed. */	       
	       settings->qfs->mms_queue_free_env(e);

	  octstr_destroy(qf);
	  octstr_destroy(transid);
	  octstr_destroy(allow_report);
	  octstr_destroy(status);
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  ctype_set = 1;
	  reply_body = octstr_imm("Received");

	  notify_cmd = "fetched";
     }
     break;
     
     case MMS_MSGTYPE_READ_REC_IND: 
     {
	  List *mh = mms_message_headers(m);
	  Octstr *from = h->client_addr ? h->client_addr : octstr_create("anon@anon");
	  List *to = gwlist_create();
	  Octstr *x;
	  

	  if (mms_convert_readrec2readorig(m) < 0)
	       goto mdone2;
	  
	  mms_collect_envdata_from_msgheaders(mh, &to,  NULL, NULL, NULL, NULL, 
					      settings->default_msgexpiry,
					      settings->max_msgexpiry,
					      NULL, NULL);
	  
	  x = settings->qfs->mms_queue_add(from, to,  NULL, NULL, NULL, time(NULL), 
					   time(NULL) + settings->default_msgexpiry, 
					   m, NULL,
					   NULL, NULL,
					   NULL, NULL,
					   qh,
					   0, 
					   octstr_get_cstr(settings->global_queuedir), 
					   "MM1",
					   settings->host_alias);
	  
	  /* Log to access log */
	  mms_log("ReadReport", h->client_addr, NULL, msize, NULL, NULL, NULL, "MM1", h->ua,NULL); 
	  octstr_destroy(x);

     mdone2:
	  http_destroy_headers(mh);
	  
	  gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  ctype_set = 1;
	  reply_body = octstr_imm("Received");
	  
	  notify_cmd = "fetched";
	  break;
     }
     break;

     /* mmbox transactions. */
     case MMS_MSGTYPE_MBOX_STORE_REQ:
     {
	  Octstr *qf = NULL, *token = NULL;
	  List *mh = mms_message_headers(m);
	  MmsMsg *mstore = NULL;
	  MmsEnvelope *e = NULL;
	  int mloc;
	  Octstr *xstate = mms_get_header_value(m, octstr_imm("X-Mms-MM-State"));
	  List *xflags = mms_get_header_values(m,  octstr_imm("X-Mms-MM-Flags"));
	  
	  Octstr *otransid = http_header_value(mh, octstr_imm("X-Mms-Transaction-ID")); 
	  Octstr *url = http_header_value(mh, octstr_imm("X-Mms-Content-Location"));
	  
	  char *mmbox_store_status = NULL;
	  Octstr *mmbox_loc = NULL;
	  Octstr *sdf = NULL;


	  if (!h->client_addr) {
	       mresp = mms_storeconf("Error-service-denied", octstr_get_cstr(otransid), NULL, 0, menc);
	       mms_error(0, "MM1", NULL,"MMS Send interface (store): failed to find sender address in request from %s!", 
		     octstr_get_cstr(h->ip));	       
	       goto store_done;
	  }
	  
	  if (mms_decodefetchurl(url, &qf, &token,&mloc) != 0) {
	       mms_error(0, "MM1", NULL,"MMS Send interface: failed to decode store url (%s) from %s!", 
		     octstr_get_cstr(url), 
		     octstr_get_cstr(h->ip));
	       mresp = mms_storeconf("Error-permanent-message-not-found", 
				    octstr_get_cstr(otransid), NULL, 0,menc);	    
	       goto store_done;
	  }

	  if (mloc == MMS_LOC_MQUEUE) { /* where is the message? */
	       e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf), 
					  octstr_get_cstr(settings->mm1_queuedir), 1);
	       
	       if (!e ||
		   (mstore = settings->qfs->mms_queue_getdata(e)) == NULL) {
		    mms_error(0, "MM1", NULL,
			  "MMS Send interface: failed to find envelope/data %s for store url "
			  "(%s) from %s (e=%s)!", 
			  octstr_get_cstr(qf), octstr_get_cstr(url), octstr_get_cstr(h->ip),
			  (e) ? "found" : "not found");  
		    
		    mresp = mms_storeconf("Error-permanent-message-not-found", 
					 octstr_get_cstr(otransid),NULL, 0,menc);	    
		    goto store_done;
	       }     
	       sdf = mms_mmbox_addmsg(octstr_get_cstr(settings->mmbox_rootdir),
				      octstr_get_cstr(h->client_addr), mstore, 
				      xflags,
				      xstate ? xstate : octstr_imm("New"));
	       
	       /* XXX perhaps qualify errors better? */
	       mmbox_store_status = sdf ? "Success" : "Error-permanent-failure";
	       if (sdf) 
		    mmbox_loc = mms_makefetchurl(octstr_get_cstr(sdf), NULL, 
						 MMS_LOC_MMBOX, h->client_addr, settings); 

	  }  else {  /* it is in mmbox, just update.  */
	       int xret;
	       
	       xret = mms_mmbox_modmsg(octstr_get_cstr(settings->mmbox_rootdir),
				       octstr_get_cstr(h->client_addr), qf, 
				       xstate,
				       xflags);
	       
	       /* XXX perhaps qualify errors better? */
	       mmbox_store_status = (xret == 0) ? "Success" : "Error-permanent-failure";
	       if (xret == 0)
		    mmbox_loc = mms_makefetchurl(octstr_get_cstr(qf), NULL, 
						 MMS_LOC_MMBOX, h->client_addr, settings); 
	       
	  }


	  if (mmbox_loc) {
	       mresp = mms_storeconf("Success", 
				     octstr_get_cstr(otransid), 
				     mmbox_loc,0,menc);
	       mms_log("Stored", h->client_addr, NULL, msize, otransid, NULL, NULL, "MM1", 
		       h->ua, sdf ? sdf : qf);
	  } else 	       
	       mresp = mms_storeconf("Error-transient-failure", 
					 octstr_get_cstr(otransid),NULL, 0,menc);	  


     store_done:

	  octstr_destroy(xstate);
	  if (xflags)
	       gwlist_destroy(xflags, (gwlist_item_destructor_t *)octstr_destroy);	  
	  mms_destroy(mstore);
	  
	  if (e) { /* Update the message queue and go. */
	       e->lastaccess = time(NULL);
	       if (settings->qfs->mms_queue_update(e) != 1) /* Should be freed. */	       
		    settings->qfs->mms_queue_free_env(e);
	       e = NULL;
	  } 
	  
	  octstr_destroy(qf);
	  octstr_destroy(token);	  
	  http_destroy_headers(mh);
	  octstr_destroy(otransid);	  
	  octstr_destroy(url);
	  octstr_destroy(mmbox_loc);
	  octstr_destroy(sdf);

	  reply_body = mms_tobinary(mresp);
	  notify_cmd = "stored";	  
     }
     break;

     case MMS_MSGTYPE_MBOX_UPLOAD_REQ:
     {
	  List *mh = mms_message_headers(m);
	  MmsMsg *mstore = NULL;
	  Octstr *ctype = NULL, *charset = NULL;
	  Octstr *otransid = http_header_value(mh, octstr_imm("X-Mms-Transaction-ID")); 
	  char *mmbox_store_status = NULL;
	  Octstr *mmbox_loc = NULL;
	  Octstr *sdf = NULL;
	  Octstr *s = NULL;
	  
	  http_header_get_content_type(mh, &ctype, &charset);

	  if (charset)
	       octstr_destroy(charset);


	  if (!h->client_addr) {
	       mresp = mms_storeconf("Error-service-denied", octstr_get_cstr(otransid), NULL, 1, menc);
	       mms_error(0, "MM1", NULL,"MMS Send interface (upload): failed to find sender address in request from %s!", 
		     octstr_get_cstr(h->ip));	       
	       goto upload_done;
	  }

	  /* If:
	   * - body type is not mms type, or 
	   * - we are unable to parse it
	   * we fail.
	   */
	  if (!ctype || 
	      octstr_case_compare(ctype, octstr_imm("application/vnd.wap.mms-message")) != 0 ||
	      (s = mms_msgbody(m)) == NULL ||
	      (mstore = mms_frombinary(s,octstr_imm("anon@anon"))) == NULL) 
	       mresp = mms_storeconf("Error-permanent-message-format-corrupt", 
				     octstr_get_cstr(otransid), 
				     NULL,1,menc);
	  else {	       
	       sdf = mms_mmbox_addmsg(octstr_get_cstr(settings->mmbox_rootdir),
				      octstr_get_cstr(h->client_addr), mstore, 
				      NULL,
				      octstr_imm("Draft"));
	       
	       /* XXX perhaps qualify errors better? */
	       mmbox_store_status = sdf ? "Success" : "Error-permanent-failure";
	       if (sdf) 
		    mmbox_loc = mms_makefetchurl(octstr_get_cstr(sdf), NULL, 
						 MMS_LOC_MMBOX, h->client_addr, settings); 
	       
	       if (mmbox_loc) {
		    mresp = mms_storeconf("Success", 
					  octstr_get_cstr(otransid), 
					  mmbox_loc,1,menc);
		    mms_log("Stored", h->client_addr, NULL, msize, otransid, NULL, NULL, "MM1", 
			    h->ua, sdf);
	       } else 	       
		    mresp = mms_storeconf("Error-transient-failure", 
					  octstr_get_cstr(otransid),NULL, 1,menc);	  
	  }

     upload_done:
	  
	  mms_destroy(mstore);	  
	  http_destroy_headers(mh);
	  octstr_destroy(otransid);
	  octstr_destroy(s);
	  octstr_destroy(ctype);	  
	  octstr_destroy(mmbox_loc);
	  octstr_destroy(sdf);
	  
	  reply_body = mms_tobinary(mresp);
	  notify_cmd = "uploaded";	  
     }
     break;
     
     case MMS_MSGTYPE_MBOX_DELETE_REQ:
     {
	  List *lh = mms_get_header_values(m, octstr_imm("X-Mms-Content-Location"));
	  Octstr *otransid = mms_get_header_value(m, octstr_imm("X-Mms-Transaction-ID")); 
	  int i;
	  List *cls = gwlist_create();
	  List *rss = gwlist_create();



	  mresp = mms_deleteconf(menc, octstr_get_cstr(otransid));

	  if (!h->client_addr) {
	       mms_error(0, "MM1", NULL,"MMS Send interface (delete): failed to find sender address in request from %s!", 
		     octstr_get_cstr(h->ip));	 
	       mms_replace_header_value(mresp, "X-Mms-Response-Status", 
					"0Error-permanent-sending-address-unresolved");
	       goto delete_done;
	  }
	  
	  for (i = 0; i < gwlist_len(lh); i++) {
	       Octstr *url = gwlist_get(lh,i);
	       int mloc;
	       Octstr *qf = NULL, *token = NULL;
	       Octstr *cl, *rs = NULL;
	       int j, m;
	       
	       cl = octstr_format("%d%S", i, url);
	       if (mms_decodefetchurl(url, &qf, &token,&mloc) != 0) {
		    mms_error(0, "MM1", NULL,"MMS Send interface: failed to decode delete url (%s) from %s!", 
			  octstr_get_cstr(url), 
			  octstr_get_cstr(h->ip));
		    rs = octstr_format("%dError-permanent-message-not-found", i);
		    
	       } else if (mloc == MMS_LOC_MQUEUE) {
		    MmsEnvelope *e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf), 
							    octstr_get_cstr(settings->mm1_queuedir), 1);
		    if (!e) 
			 rs = octstr_format("%dError-permanent-message-not-found", i);			 
		    else {
			 for (j = 0, m = (e->to ? gwlist_len(e->to) : 0); j < m; j++) {
			      MmsEnvelopeTo *x = gwlist_get(e->to,j);
			      if (x) x->process = 0;
			 }
			 if (settings->qfs->mms_queue_update(e) != 1) /* Should be freed. */	       
			      settings->qfs->mms_queue_free_env(e);			 
			 rs = octstr_format("%dOk", i);			 
		    }
	       } else  if (mloc == MMS_LOC_MMBOX) {
		    int ret2 = mms_mmbox_delmsg(octstr_get_cstr(settings->mmbox_rootdir),
						octstr_get_cstr(h->client_addr),qf);
		    if (ret2 == 0) 
			 rs = octstr_format("%dOk", i);			 			 
		    else /* XXX better error reporting... */
			 rs = octstr_format("%dError-permanent-message-not-found", i);		    
	       }

	       if (!rs)
		    rs = octstr_format("%dError-permanent-message-not-found", i);

	       gwlist_append(rss, rs);
	       gwlist_append(cls, cl);	       

	       if (qf)
		    octstr_destroy(qf);
	       if (token)
		    octstr_destroy(token);
	  }

	  /* put in response codes. */
	  mms_replace_header_values(mresp, "X-Mms-Content-Location", cls);
	  mms_replace_header_values(mresp, "X-Mms-Response-Status", rss);

     delete_done:
	  gwlist_destroy(cls, (gwlist_item_destructor_t *)octstr_destroy);     
	  gwlist_destroy(rss, (gwlist_item_destructor_t *)octstr_destroy);     
	  gwlist_destroy(lh, (gwlist_item_destructor_t *)octstr_destroy);     

	  octstr_destroy(otransid);

	  reply_body = mms_tobinary(mresp);
	  notify_cmd = "deleted";	  
     }
     break;
     
     case MMS_MSGTYPE_MBOX_VIEW_REQ:
       {
	    /* Get the search params, search, build response, go. */
	    Octstr *otransid = mms_get_header_value(m, octstr_imm("X-Mms-Transaction-ID")); 
	    List *xstates = mms_get_header_values(m, octstr_imm("X-Mms-MM-State"));
	    List *xflags = mms_get_header_values(m,  octstr_imm("X-Mms-MM-Flags"));
	    List *msgs = mms_get_header_values(m,  octstr_imm("X-Mms-Content-Location"));
	    List *required_headers = mms_get_header_values(m,  octstr_imm("X-Mms-Attributes"));
	    List *otherhdrs = http_create_empty_headers();

	    List *msgrefs = NULL, *msglocs = NULL;
	    char *err = "Ok";	    
	    Octstr *x;
	    int start, limit;
	    MmsUaProfile *prof = NULL;	    

	  if (!h->client_addr) {
	       mms_error(0, "MM1", NULL,"MMS Send interface (view): failed to find sender address in request from %s!", 
		     octstr_get_cstr(h->ip));	 

	       err = "Error-permanent-sending-address-unresolved";
	       goto view_done;
	  }

	    if ((x = mms_get_header_value(m, octstr_imm("X-Mms-Start"))) != NULL) {
		 sscanf(octstr_get_cstr(x), "%d", &start);
		 octstr_destroy(x);
	    } else
		 start = 0;

	    if ((x = mms_get_header_value(m, octstr_imm("X-Mms-Limit"))) != NULL) {
		 sscanf(octstr_get_cstr(x), "%d", &limit);
		 octstr_destroy(x);
	    } else 
		 limit = USER_MMBOX_MSG_QUOTA;	
    
	    /* Check for quota and count requests. */
	    if ((x = mms_get_header_value(m, octstr_imm("X-Mms-Totals"))) != NULL) {
		 if (octstr_case_compare(x, octstr_imm("Yes")) == 0) {
		      unsigned long byte_count  = 0, msg_count = 0;
		      char y[64];
		      mms_mmbox_count(octstr_get_cstr(settings->mmbox_rootdir),
				      octstr_get_cstr(h->client_addr), 
				      &msg_count, &byte_count);
		      
		      sprintf(y, "%d bytes", (int)byte_count);
		      http_header_add(otherhdrs, "X-Mms-Mbox-Totals", y);
		      
		      sprintf(y, "%d msgs", (int)msg_count);
		      http_header_add(otherhdrs, "X-Mms-Mbox-Totals", y);
		      
		 }
		 octstr_destroy(x);
	    }

	    if ((x = mms_get_header_value(m, octstr_imm("X-Mms-Quotas"))) != NULL) {
		 if (octstr_case_compare(x, octstr_imm("Yes")) == 0) { /* will we ever implement message quota?? */ 
		      char y[64];
		      
		      sprintf(y, "%d bytes", USER_MMBOX_DATA_QUOTA);
		      http_header_add(otherhdrs, "X-Mms-Mbox-Quotas", y);
		 }
		 octstr_destroy(x);
	    }
	    
	    /* Should we add the filter and limit headers to otherhdrs? 
	     * why bother when send knows them? 
	     */
	    if (h->profile_url && 
		(prof = mms_get_ua_profile(octstr_get_cstr(h->profile_url))) != NULL) {
		 int i, n;

		 for (i = 0, n = msgs ? gwlist_len(msgs) : 0; i<n; i++) { /* Make message references. */
		      Octstr *x = gwlist_get(msgs, i);
		      Octstr *sdf = NULL, *token = NULL;
		      int loc;
		      
		      if (mms_decodefetchurl(x, &sdf, &token, &loc) == 0) {
			   gwlist_insert(msgs, i, sdf);
			   gwlist_delete(msgs, i+1, 1);			   
			   octstr_destroy(x);
		      } else { 
			   if (sdf)
				octstr_destroy(sdf);
		      }
		      
		      if (token) octstr_destroy(token);
		 }
		 
		 msgrefs = mms_mmbox_search(octstr_get_cstr(settings->mmbox_rootdir),
					    octstr_get_cstr(h->client_addr), xstates, 
					    xflags, start, limit, msgs);
		 /* Make the locations. */
		 msglocs = gwlist_create();
		 for (i = 0, n = gwlist_len(msgrefs); i < n; i++) {
		      Octstr *sdf = gwlist_get(msgrefs, i);
		      gwlist_append(msglocs, 
				  mms_makefetchurl(octstr_get_cstr(sdf), 
						   NULL, MMS_LOC_MMBOX, h->client_addr, settings)); 
		 }
	    } else /* Profile not loaded... */
		 err = "Error-transient-network-problem";

       view_done:
	    mresp = mms_viewconf(octstr_get_cstr(otransid), 
				 msgrefs,
 				 msglocs,
				 err, 
				 required_headers,
				 (MmsMsgGetFunc_t *)mms_mmbox_get, 
				 octstr_get_cstr(settings->mmbox_rootdir),
				 octstr_get_cstr(h->client_addr),
				 prof ? mms_ua_maxmsgsize(prof) : MAX_MESSAGE_SIZE,
				 MS_1_2, 				 
				 otherhdrs); 
	    reply_body = mms_tobinary(mresp);
	    notify_cmd = "deleted"; 
	    
	    
	    gwlist_destroy(xflags, (gwlist_item_destructor_t *)octstr_destroy);	   
	    gwlist_destroy(xstates, (gwlist_item_destructor_t *)octstr_destroy);
	    gwlist_destroy(required_headers, (gwlist_item_destructor_t *)octstr_destroy);	    
	    gwlist_destroy(msgrefs, (gwlist_item_destructor_t *)octstr_destroy);
	    gwlist_destroy(msglocs, (gwlist_item_destructor_t *)octstr_destroy);
	    gwlist_destroy(msgs, (gwlist_item_destructor_t *)octstr_destroy);
	    http_destroy_headers(otherhdrs);
	    octstr_destroy(otransid);
       }
       break;

     default:
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  ctype_set = 1;
	  hstatus = HTTP_FORBIDDEN;
	  xmtype = (void *)mms_message_type_to_cstr(mtype);
	  reply_body = octstr_format("Unexpected MMS message type %s [%d]", 
				     xmtype ? xmtype : "(null)", mtype);
	  break;
     }
     

     if (notify_cmd) /* Inform provisioning server */	  
	  notify_prov_server(octstr_get_cstr(settings->prov_notify), 
			     h->base_client_addr ? octstr_get_cstr(h->base_client_addr) : "unknown", 
			     notify_cmd, 
			     "", NULL,
			     h->ua, h->profile_url);

     mms_destroy(m);
     /* Send reply. */

 done:

#if 0
     if (mresp) 
	  mms_msgdump(mresp, 0);
#endif
     
     
     mms_destroy(mresp);
     if (!ctype_set)
	  http_header_add(rh, "Content-Type", "application/vnd.wap.mms-message");
     
     http_send_reply(h->client, hstatus, rh, reply_body);
     http_destroy_headers(rh);
     octstr_destroy(reply_body);
     http_destroy_headers(qh);
     free_clientInfo(h,1);     
}

/* Find sender credentials: only auth-basic supported for now. */
static MmsVasp *find_mm7sender(List *headers, Dict *vasps, int *has_auth)
{
     Octstr *v = http_header_value(headers, octstr_imm("Authorization"));
     Octstr *p = NULL, *q = NULL, *x;
     MmsVasp *m =  NULL;
     List *vlist;
     int i;
     
     *has_auth = (v != NULL);

     if (!v || 
	 octstr_search(v, octstr_imm("Basic "), 0) != 0)
	  goto done;
     p = octstr_copy(v, sizeof "Basic", octstr_len(v));
     octstr_base64_to_binary(p);
     
     i = octstr_search_char(p, ':', 0);
     q = octstr_copy(p, i+1, octstr_len(p));
     octstr_delete(p, i, octstr_len(p));
     
     /* p = user, q = pass. */
     vlist = dict_keys(vasps);
     while ((x = gwlist_extract_first(vlist)) != NULL) {
	  MmsVasp *mv = dict_get(vasps, x);

	  octstr_destroy(x);	  

	  if (mv && 
	      octstr_compare(mv->vasp_username, p) == 0 &&
	      octstr_compare(mv->vasp_password, q) == 0) {
	       m = mv;
	       break;
	  }
     }
     gwlist_destroy(vlist, NULL);
     
     /* if it can't authenticate, returns NULL. */
     
 done:
     octstr_destroy(v);
     octstr_destroy(p);
     octstr_destroy(q);

     return m;
}

static void mm7soap_dispatch(MmsHTTPClientInfo *h)
{
     /* if no vasp, return 4001 error. */
     MSoapMsg_t *mreq = NULL, *mresp = NULL;
     int hstatus = HTTP_OK;
     List *rh = NULL;
     Octstr *reply_body = NULL;
     Octstr *sender = NULL;
     MmsEnvelope *e = NULL;
     
     List *to = NULL;
     Octstr *from = NULL, *subject = NULL,  *vasid = NULL, *msgid = NULL;
     time_t expiryt = -1, delivert = -1;
     MmsMsg *m = NULL;
     int status = 1000;
     unsigned char *msgtype = (unsigned char *)"";
     Octstr *qf = NULL;
     

     if (h->body)     
	  mreq = mm7_parse_soap(h->headers, h->body);
     if (mreq)
	  msgtype = mms_mm7tag_to_cstr(mm7_msgtype(mreq));
     debug("mmsprox.mm7sendinterface", 0,
	   " --> Enterred mm7dispatch interface, mreq=[%s] mtype=[%s] <-- ",
	   mreq ? "Ok" : "Null",
	   mreq ? (char *)msgtype : "Null");

     if (!mreq) {
	  status = MM7_SOAP_FORMAT_CORRUPT;
	  mresp = mm7_make_resp(NULL, status, NULL,0);
	  goto done;
     } 
     
     h->vasp->stats.mo_pdus++;               
     h->vasp->stats.last_pdu = time(NULL);

     sender = octstr_format("%d/TYPE=PLMN", h->vasp->short_codes[0]); /* defaults to first short code. */
     switch (mm7_msgtype(mreq)) {
     case MM7_TAG_SubmitReq:
	  mm7_get_envelope(mreq, &from, &to, &subject, &vasid, &expiryt, &delivert, NULL, NULL);
	  m = mm7_soap_to_mmsmsg(mreq,  from ? from : sender); 
	  if (m) {
	       Octstr *value = NULL;
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
		    expiryt = time(NULL) + settings->default_msgexpiry;
	       mms_remove_headers(m, "Message-ID"); /* cannot be found here. */
	       qf = settings->qfs->mms_queue_add(from ? from : sender, to, subject, 
						 NULL, NULL, 
						 delivert, expiryt, m, NULL, 
						 h->vasp->id, vasid, 
						 NULL, NULL,
						 NULL,
						 dlr, 
						 octstr_get_cstr(settings->global_queuedir), 
						 "MM7",
						 settings->host_alias);
	       msgid = mms_make_msgid(octstr_get_cstr(qf), settings->host_alias);
	       mms_log("Received", from ? from : sender, to, -1, msgid, h->vasp->id, NULL, "MM7", 
		       h->ua, NULL);
	       octstr_destroy(value);	      
	  }  else {
	       mms_error(0, "MM7", NULL,"Failed to convert received MM7/SOAP SubmitReq message from vasp=%s to MMS Message!",
		     octstr_get_cstr(h->vasp->id));	       
	       status = 4004;	  
	  }
	  mresp = mm7_make_resp(mreq, status, msgid,0);
	  break; 

     case MM7_TAG_ReplaceReq:
	  msgid = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
	  if (msgid && (qf = mms_getqf_from_msgid(msgid)) != NULL && 
	      (e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf), 
					  octstr_get_cstr(settings->global_queuedir), 
					  1)) != NULL) {
	       if (!e->vaspid ||
		   octstr_compare(e->vaspid, h->vasp->id) != 0) {
		    status = 2001;
		    mms_error(0, "MM7", NULL,"MMS Proxy(MM7): ReplaceReq: Found message[id=%s]"
			  " but vaspid id=%s does not match!",
			  octstr_get_cstr(msgid), octstr_get_cstr(h->vasp->id));		    
	       } else { /* get orig message, change headers of new, replace old. */
		    MmsMsg *old = settings->qfs->mms_queue_getdata(e);
		    MmsMsg *new = mm7_soap_to_mmsmsg(mreq, sender);
		    List *hh = mms_message_headers(old);
		    Octstr *s;
		    
		    if (new) {
			 mms_add_missing_headers(new, hh);
			 if (settings->qfs->mms_queue_replacedata(e, new) < 0) {
			      status = 3000;
			      mms_error(0, "MM7", NULL, "MMS Proxy(MM7): ReplaceReq: Failed to change data, "
				    "id=%s, vasp=%s!",
				    msgid ? octstr_get_cstr(msgid) : "NULL",
				    octstr_get_cstr(h->vasp->id));		    
			 }
		    } else
			 mms_warning(0,  "MM7", NULL,"MMS Proxy(MM7): ReplaceReq: No data sent??, "
				 "id=%s, vasp=%s!",
				 msgid ? octstr_get_cstr(msgid) : "NULL",
				 octstr_get_cstr(h->vasp->id));		    
		    
		    if ((s = mm7_soap_header_value(mreq, 
						   octstr_imm("EarliestDeliveryTime"))) != NULL) {
			 e->sendt = date_parse_http(s);
			 octstr_destroy(s);
		    }
		      
		    if (settings->qfs->mms_queue_update(e) != 1)
			 settings->qfs->mms_queue_free_env(e);
		    e = NULL;
		    mms_log("Replace", 
			    sender, NULL, -1, msgid, h->vasp->id, NULL, "MM7", h->ua, NULL);
		    
		    mms_destroy(new);
		    mms_destroy(old);
		    http_destroy_headers(hh);
	       }
	  } else {
	       status = 2005;
	       mms_error(0, "MM7", NULL,"MMS Proxy(MM7): ReplaceReq: Failed to find msg, id=%s, vasp=%s!",
		     msgid ? octstr_get_cstr(msgid) : "NULL",
		     octstr_get_cstr(h->vasp->id));		    
	  }
	  
	  mresp = mm7_make_resp(mreq, status, NULL,0);
	  break;
	  
     case MM7_TAG_CancelReq:
	  msgid = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
	  if (msgid && (qf = mms_getqf_from_msgid(msgid)) != NULL && 
	      (e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf), 
					  octstr_get_cstr(settings->global_queuedir), 
					  1)) != NULL) {
	       if (!e->vaspid ||
		   octstr_compare(e->vaspid, h->vasp->id) != 0) {
		    status = 2001;
		    mms_error(0, "MM7", NULL,"MMS Proxy(MM7): CancelReq: Found message[id=%s]"
			  " but vaspid id=%s does not match!",
			  octstr_get_cstr(msgid), octstr_get_cstr(h->vasp->id));		    
	       } else { /* Kill it. */
		    int i, n;
		    for (i = 0, n = gwlist_len(e->to); i<n; i++) {
			 MmsEnvelopeTo *xto = gwlist_get(e->to,i);			 
			 xto->process = 0;
		    }
		    settings->qfs->mms_queue_update(e); /* Will clear it. */
		    e = NULL;
		    mms_log("Cancel", 
			    sender, NULL, -1, msgid, h->vasp->id, NULL, "MM7", h->ua, NULL);
	       }
	  } else {
	       status = 2005;
	       mms_error(0, "MM7", NULL,"MMS Proxy(MM7): CancelReq: Failed to find msg, id=%s, vasp=%s!",
		     msgid ? octstr_get_cstr(msgid) : "NULL",
		     octstr_get_cstr(h->vasp->id));		    
	  }
	  mresp = mm7_make_resp(mreq, status, NULL,0);
	  break;
     default:
	  status = MM7_SOAP_UNSUPPORTED_OPERATION;
	  mresp = mm7_make_resp(mreq, status, NULL,0);
	  break;	  
     }
     
 done:

     if (!MM7_SOAP_STATUS_OK(status))
	  h->vasp->stats.mo_errors++; /* increment error count. */

     if (mresp && mm7_soapmsg_to_httpmsg(mresp, &h->vasp->ver, &rh, &reply_body) == 0) 
	  http_send_reply(h->client, hstatus, rh, reply_body);
     else 
	  http_close_client(h->client);

     debug("mmsprox.mm7sendinterface", 0,
	   " --> leaving mm7dispatch interface, mresp=[%s], body=[%s] <-- ",
	   mresp ? "ok" : "(null)",
	   reply_body ? "ok" : "(null)");
     
     settings->qfs->mms_queue_free_env(e);
     octstr_destroy(sender);
     octstr_destroy(from);
     octstr_destroy(subject);
     octstr_destroy(vasid);
     octstr_destroy(msgid);
     octstr_destroy(qf);
     mms_destroy(m);
     http_destroy_headers(rh);     
     octstr_destroy(reply_body);
     mm7_soap_destroy(mresp);
     mm7_soap_destroy(mreq);
     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
     
     free_clientInfo(h,1);
}

static void mm7eaif_dispatch(MmsHTTPClientInfo *h)
{
     MmsMsg *m = NULL;
     List *mh = NULL;
     int hstatus = HTTP_NO_CONTENT;
     List *rh = http_create_empty_headers();
     Octstr *reply_body = NULL, *value, *xver;
     
     List *to = gwlist_create(), *hto = NULL;
     Octstr *subject = NULL,  *otransid = NULL, *msgid = NULL;
     Octstr *hfrom = NULL;
     time_t expiryt = -1, deliveryt = -1;
     Octstr *qf = NULL;
     int msize = h->body ? octstr_len(h->body) : 0;
     int dlr;
     
     debug("mm7eaif.sendinterface", 0, 
	   " --> Enterred eaif send interface, blen=%d <--- ", 
	   msize);

     hfrom = http_header_value(h->headers, octstr_imm("X-NOKIA-MMSC-From"));     
     if (!h->body ||  /* A body is required, and must parse */
	 (m = mms_frombinary(h->body, hfrom ? hfrom : octstr_imm("anon@anon"))) == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Unexpected MMS message, no body?");
	  
	  goto done;
     }      

     h->vasp->stats.mo_pdus++;
     h->vasp->stats.last_pdu = time(NULL);

     mms_remove_headers(m, "Message-ID");
     mh = mms_message_headers(m);
     /* Now get sender and receiver data. 
      * for now we ignore adaptation flags. 
      */
     mms_collect_envdata_from_msgheaders(mh, &to, &subject, &otransid, &expiryt, 
					 &deliveryt, settings->default_msgexpiry,
					 settings->max_msgexpiry,
					 NULL, NULL);
     

     if ((hto = http_header_find_all(h->headers, "X-NOKIA-MMSC-To")) != NULL && 
	 gwlist_len(hto) > 0) { /* To address is in headers. */
	  int i, n;
	  
	  if (to) 
	       gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
	  to = gwlist_create();
	  for (i = 0, n = gwlist_len(hto); i < n; i++) {
	       Octstr *h = NULL, *v = NULL;
	       List *l;
	       int j, m;

	       http_header_get(hto,i,  &h, &v);	       
	       l = http_header_split_value(v);
	       
	       for (j = 0, m = gwlist_len(l); j < m; j++)
		    gwlist_append(to, gwlist_get(l, j));
	       
	       gwlist_destroy(l, NULL);
	       if (h) octstr_destroy(h);	       
	       if (v) octstr_destroy(v);	       	       
	  }
	  
     }

     value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"));	  
     if (value && 
	 octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	  dlr = 1;
     else 
	  dlr = 0;
     if (value)
	  octstr_destroy(value);
     
     if (deliveryt < 0)
	  deliveryt = time(NULL);
     
     if (expiryt < 0)
	  expiryt = time(NULL) + settings->default_msgexpiry;

     if (hfrom == NULL)
	  hfrom = http_header_value(mh, octstr_imm("From"));
     
     mms_remove_headers(m, "Bcc");
     mms_remove_headers(m, "X-Mms-Delivery-Time");
     mms_remove_headers(m, "X-Mms-Expiry");
     mms_remove_headers(m, "X-Mms-Sender-Visibility");
	      
     /* Save it, make msgid, put message id in header, return. */     
     qf = settings->qfs->mms_queue_add(hfrom, to, subject, 
				       NULL, NULL, deliveryt, expiryt, m, NULL, 
				       NULL, NULL,
				       NULL, NULL,
				       NULL,
				       dlr,
				       octstr_get_cstr(settings->global_queuedir), 
				       "MM7",
				       settings->host_alias);
	      
     if (qf) {
	  msgid = mms_make_msgid(octstr_get_cstr(qf), 
				  settings->host_alias);		  
	  
	  /* Log to access log */
	  mms_log("Received", hfrom, to, msize, msgid, h->vasp->id, NULL, "MM7", h->ua, NULL);
	  
	  octstr_destroy(qf);
	  hstatus = HTTP_NO_CONTENT;
     } else 
	  hstatus = HTTP_INTERNAL_SERVER_ERROR;
     
 done:
     xver = octstr_format(EAIF_VERSION, h->vasp->ver.major, h->vasp->ver.minor1);
     http_header_add(rh, "X-NOKIA-MMSC-Version", octstr_get_cstr(xver));
     octstr_destroy(xver);

     if (msgid)
	  http_header_add(rh, "X-NOKIA-MMSC-Message-Id", octstr_get_cstr(msgid));

     http_send_reply(h->client, hstatus, rh, octstr_imm(""));

     if (http_status_class(hstatus) != HTTP_OK)
	  h->vasp->stats.mo_errors++;
     
     http_destroy_headers(hto);
     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
     octstr_destroy(hfrom);
     octstr_destroy(subject);
     octstr_destroy(otransid);
     octstr_destroy(msgid);
     http_destroy_headers(mh);     
     mms_destroy(m);              
}

static void mm7proxy(void *unused)
{
     MmsHTTPClientInfo h = {NULL};
     while(rstop == 0 && (h.client = http_accept_request(settings->mm7port, 
					   &h.ip, &h.url, &h.headers, 
					   &h.body, &h.cgivars)) != NULL) 
	  if (is_allowed_ip(settings->allow_ip, settings->deny_ip, h.ip)) {
	       MmsHTTPClientInfo *hx = gw_malloc(sizeof *hx);
	       int has_auth = 0;
	       double tdur;
	       
	       /* Clear some stuff. */
	       h.client_addr = NULL;
	       h.base_client_addr = NULL;
	       h.profile_url = NULL;

	       /* Get the MM7 sender address. */	       
	       h.vasp = find_mm7sender(h.headers, settings->vasp_list, &has_auth);  	       
	       h.ua = http_header_value(h.headers, octstr_imm("User-Agent"));	       
	       debug("mmsproxy", 0, 
		     " MM7 Request, ip=%s, vasp=%s ", 
		     h.ip ? octstr_get_cstr(h.ip) : "", 
		     h.vasp && h.vasp->id ? octstr_get_cstr(h.vasp->id) : "(null)");  
	       
	
#if 0
       /* Dump headers, url etc. */
	       http_header_dump(h.headers);
	       if (h.body) octstr_dump(h.body, 0);
	       if (h.ip) octstr_dump(h.ip, 0);
#endif
	       *hx = h; /* Copy it all over. */
	       if (!h.vasp) { /* Ask it to authenticate... */
		    List *hh = http_create_empty_headers();
		    http_header_add(hh, "WWW-Authenticate", 
				    "Basic realm=\"" MM_NAME "\"");
		    http_send_reply(hx->client, HTTP_UNAUTHORIZED, hh, 
				    octstr_imm(""));			   
		    http_destroy_headers(hh);
		    free_clientInfo(hx, 1);
		    if (!has_auth)
			 mms_info_ex("auth", 0, "MM7", NULL, "MMSC: Auth failed/VASP not found in MM7 incoming connection!");
		    else
			 mms_error_ex("auth", 0, "MM7", NULL, "MMSC: Auth failed/VASP not found in MM7 incoming connection!");		    
	       } else if (h.vasp->throughput > 0 && 
			  (tdur = time(NULL) - h.vasp->stats.start_time) > 0 && 
			  h.vasp->stats.mo_pdus/tdur > h.vasp->throughput) { /* throttling error, do not even process the message. */
		    List *hh = http_create_empty_headers();
		    http_send_reply(hx->client, 409, hh, /* respond with HTTP Conflict code. */
				    octstr_imm("Throttling limit exceeded. Try again later"));			   
		    http_destroy_headers(hh);
		    free_clientInfo(hx, 1);	
		    mms_error_ex("throttle", 0, "MM7", h.vasp->id, 
				 "MMSC: VASP Exceeded throttling limit (%.2f PDU/sec). Connection reset!", 
			 h.vasp->throughput);		    		    	    
	       } else if (h.vasp->type == SOAP_VASP)
		    gwthread_create((gwthread_func_t *)mm7soap_dispatch, hx);
	       else 
		    gwthread_create((gwthread_func_t *)mm7eaif_dispatch, hx);
	  } else {	  
	       http_close_client(h.client);
	       free_clientInfo(&h, 0);
	  }
     
     debug("proxy", 0, "MM7 Shutting down...");          
}

static void free_clientInfo(MmsHTTPClientInfo *h, int freeh)
{

     debug("free info", 0,
	   " entered free_clientinfo %d, ip=%ld", freeh, (long)h->ip);
	  
     octstr_destroy(h->ip);
     octstr_destroy(h->url);    
     octstr_destroy(h->ua); 
     octstr_destroy(h->body);
     octstr_destroy(h->base_client_addr);
     octstr_destroy(h->client_addr);
     http_destroy_cgiargs(h->cgivars);
     http_destroy_headers(h->headers);
     octstr_destroy(h->profile_url);

     if (freeh)
	  gw_free(h);    

     debug("free info", 0,
	   " left free_clientinfo");     
}

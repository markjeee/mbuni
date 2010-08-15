/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Email2MMS and MM4 (incoming) interface
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include <signal.h>
#include "mms_queue.h"
#include "mmsc_cfg.h"

static MmscSettings *settings;

static Octstr *xfrom;
static Octstr *xto;
static Octstr *xproxy;

enum {TPLMN, TEMAIL, TOTHER} ttype;
/* MM4 message types. */
enum {MM4_FORWARD_REQ = 0, MM4_FORWARD_RES, MM4_READ_REPLY_REPORT_REQ, 
      MM4_READ_REPLY_REPORT_RES, MM4_DELIVERY_REPORT_REQ,  MM4_DELIVERY_REPORT_RES};
static struct {
     char *mm4str;
     int mm1_type;
} mm4_types[] = {
     {"MM4_forward.REQ", MMS_MSGTYPE_SEND_REQ},
     {"MM4_forward.RES",-1},
     {"MM4_read_reply_report.REQ", MMS_MSGTYPE_READ_REC_IND},
     {"MM4_read_reply_report.RES",-1},
     {"MM4_delivery_report.REQ",MMS_MSGTYPE_DELIVERY_IND},
     {"MM4_delivery_report.RES",-1},
     {NULL}
};
/* above indexed by types! */

static int find_own(int i, int argc, char *argv[]);

static void fixup_recipient(Octstr **host);
static void fixup_sender(void);
static void fixup_addresses(List *headers);

static void send_mm4_res(int mtype, Octstr *to, Octstr *sender, Octstr *transid, char *status, Octstr *msgid, 
     Octstr *sendmail_cmd);

static void strip_quotes(Octstr *s);

static List *proxyrelays;
static int no_strip = 0, strip_type = 0;

int main(int argc, char *argv[])
{
     MIMEEntity *mm;
     MmsMsg *msg;

     Octstr *email, *me, *rstatus, *fname;
     Octstr *home_mmsc = NULL, *rhost = NULL;
     List *headers, *h2;
     Octstr *mm4_type = NULL, *transid, *ack, *msgid, *orig_sys;
     Octstr *newmsgid = NULL;

     int mtype = -1, mm1_type = -1, cfidx, i;
     char *err = NULL;

     mms_lib_init();

     cfidx = get_and_set_debugs(argc, argv, find_own);

     if (argv[cfidx] == NULL)
	  fname = octstr_imm("mmsc.conf");
     else 
	  fname = octstr_create(argv[cfidx]);

     mms_info(0, "mmsfromemail", NULL, "----------------------------------------");
     mms_info(0, "mmsfromemail", NULL, " MMSC Email2MMS/MM4 Incoming Tool  version %s starting", MMSC_VERSION);

     /* Load settings. */     
     settings = mms_load_mmsc_settings(fname, &proxyrelays,1);                        
     if (!settings) 
	  panic(0, "No global MMSC configuration, or failed to read conf from <%s>!", octstr_get_cstr(fname));
     
     octstr_destroy(fname);     
     if (!xto || !xfrom) {
	  mms_error(0, "mmsfromemail", NULL, "usage: %s -f from -t to!", argv[0]);
	  return -1;
     }

     /* normalize recipient address, then  if phone number, 
      * check whether we are allowed to process. 
      */
     fixup_recipient(&rhost);     
     fixup_sender();

     if (xto && ttype == TPLMN) /* Get the home mmsc domain for this recipient. */
	  home_mmsc = settings->mms_resolvefuncs->mms_resolve(xto, 
							      "MM4", xproxy  ? octstr_get_cstr(xproxy) : NULL,
							      settings->mms_resolver_module_data,
							      settings, proxyrelays);
     if (!xto ||	
	 (ttype == TPLMN && (!home_mmsc || 
			     !is_allowed_host(home_mmsc, 
					      settings->email2mmsrelay_hosts)))) {
	  mms_error(0, "mmsfromemail", NULL, " Not allowed to send to this recipient %s, resolved mmsc=%s!", 
		xto ? octstr_get_cstr(xto) : "(null)",
		home_mmsc ? octstr_get_cstr(home_mmsc) : "(null)");
	  mms_lib_shutdown();
	  return -1;
     }

     
     email = octstr_read_pipe(stdin);

     if (!email || octstr_len(email) == 0) {
	  mms_error(0, "mmsfromeail", NULL, "Empty email message!");
	  mms_lib_shutdown();
	  return -1;
     } 
     
     if ((mm = mime_octstr_to_entity(email)) == NULL) {
	  mms_error(0, "mmsfromeail", NULL, "Unable to decode mime entity!");
	  mms_lib_shutdown();
	  return -1;
     }
     octstr_destroy(email);

     /* Take the entity, recode it --> remove base64 stuff, split headers. */
     unbase64_mimeparts(mm);
     unpack_mimeheaders(mm);
     
     /* Delete some headers... */
     headers = mime_entity_headers(mm);
     http_header_remove_all(headers, "Received");
     http_header_remove_all(headers, "X-MimeOLE");
     http_header_remove_all(headers, "X-Mailer");

     /* rebuild headers, removing nasty looking ones. */
     h2 = http_create_empty_headers();
     for (i = 0; i<gwlist_len(headers); i++) {
	  Octstr *name = NULL, *value = NULL;
	  http_header_get(headers, i, &name, &value);
	  
	  if (!name ||
	      octstr_case_search(name, octstr_imm("spam"), 0) >= 0 ||
	      octstr_case_search(name, octstr_imm("mailscanner"), 0) >= 0)
	       goto loop;
	  
	  http_header_add(h2, octstr_get_cstr(name), octstr_get_cstr(value));
     loop:
	  octstr_destroy(name);
	  octstr_destroy(value);	  
     }
     http_destroy_headers(headers);
     headers = h2;
     /* Look for MM4 headers... */
     mm4_type = http_header_value(headers, octstr_imm("X-Mms-Message-Type"));
     ack = http_header_value(headers, octstr_imm("X-Mms-Ack-Request"));
     rstatus = http_header_value(headers, octstr_imm("X-Mms-Request-Status-Code"));

     if ((transid = http_header_value(headers, octstr_imm("X-Mms-Transaction-ID"))) == NULL)
	  transid = octstr_create("001");
     /* get originator system. */
     if ((orig_sys = http_header_value(headers, octstr_imm("X-Mms-Originator-System"))) == NULL) 
	  orig_sys = http_header_value(headers, octstr_imm("Sender"));
     
     if (xproxy == NULL && orig_sys != NULL) { /* Copy proxy address from originator system. */
	  int i = octstr_search_char(orig_sys, '@', 0);
	  if (i >= 0)
	       xproxy = octstr_copy(orig_sys, i+1, octstr_len(orig_sys));
     }
     
     if ((msgid =  http_header_value(headers, octstr_imm("X-Mms-Message-ID"))) == NULL)
	  msgid = http_header_value(headers, octstr_imm("Message-ID"));

     strip_quoted_string(msgid);     
     strip_quoted_string(transid);
     

     mms_info(0, "MM4", NULL, "Received [message type: %s] [transaction id: %s] [origin: %s] [msgid: %s]",
	  mm4_type ? octstr_get_cstr(mm4_type) : "",
	  transid ? octstr_get_cstr(transid) : "",
	  orig_sys ? octstr_get_cstr(orig_sys) : "",
	  msgid ? octstr_get_cstr(msgid) : "");

     /* ... and remove non-essential ones */
     http_header_remove_all(headers, "X-Mms-3GPP-MMS-Version");
     http_header_remove_all(headers, "MIME-Version");
     http_header_remove_all(headers, "X-Mms-Message-ID");
     http_header_remove_all(headers, "Message-ID");
     http_header_remove_all(headers, "X-Mms-Ack-Request");
     http_header_remove_all(headers, "X-Mms-Originator-System");

     http_header_remove_all(headers, "Sender");
     
     /* msgid was there, put it back in proper form. */
     if (msgid)
	  http_header_add(headers, "Message-ID", octstr_get_cstr(msgid));
     
     fixup_addresses(headers);
     
     if (mm4_type) {
	  unsigned char *x = NULL;
	  Octstr *y;
	  int i;

	  http_header_remove_all(headers, "X-Mms-Message-Type");
	  for (i = 0; mm4_types[i].mm4str; i++)
	       if (octstr_str_case_compare(mm4_type, mm4_types[i].mm4str) == 0) {
		    mtype = i;
		    mm1_type = mm4_types[i].mm1_type;
		    x = mms_message_type_to_cstr(mm1_type);
		    break;
	       }

	  if (x) {
	       http_header_add(headers, "X-Mms-Message-Type", (char *)x);  
	       if (orig_sys == NULL) /* Make it up! */
		    orig_sys = octstr_format("system-user@%S", 
					     xproxy ? xproxy : octstr_imm("unknown"));	       
	  } else {
	       octstr_destroy(mm4_type);     
	       mm4_type = NULL; /* So that we assume normal message below. */
	  }
	  
	  if ((y = http_header_value(headers, octstr_imm("X-Mms-MM-Status-Code"))) != NULL) {
	       /* This field is different on MM1. */
	       http_header_remove_all(headers, "X-Mms-MM-Status-Code");
	       http_header_add(headers, "X-Mms-Status", octstr_get_cstr(y));
	       octstr_destroy(y);
	  }
     } 

     if (mm4_type == NULL) { /* else assume a normal send message. */
	  http_header_add(headers, "X-Mms-Message-Type", "m-send-req");  
	  mm1_type = MMS_MSGTYPE_SEND_REQ;
	  mtype = MM4_FORWARD_REQ;
     }
     
     mime_replace_headers(mm, headers);
     http_destroy_headers(headers);

     /* Now convert from mime to MMS message, if we should */
     if (mm1_type >= 0) {
	  if ((msg = mms_frommime(mm)) == NULL) {
	       mms_error(0, "mmsfromeail", NULL, "Unable to create MM!");
	       mms_lib_shutdown();
	       return -1;
	  }
     } else 
	  msg = NULL;     
     mime_entity_destroy(mm);     
     me = octstr_format("system-user@%S", settings->hostname);
     
     switch(mtype) {	  
     case MM4_FORWARD_REQ:
	  if (ttype != TPLMN && settings->mms2email == NULL) {
	       err = "Error-service-denied";
	       mms_error(0, "MM4", NULL, "Not allowed to send to non-phone recipient, to=%s!", octstr_get_cstr(xto));
	  } else {
	       List *lto = gwlist_create();
	       Octstr *qf;
	       Octstr *dreport = mms_get_header_value(msg, octstr_imm("X-Mms-Delivery-Report"));

	       int dlr;
	       
	       if (ttype == TPLMN)
		    octstr_format_append(xto, "/TYPE=PLMN");
	       else 
		    octstr_format_append(xto, "@%S", rhost);
	       gwlist_append(lto, xto);

	       if (dreport && 
		   octstr_case_compare(dreport, octstr_imm("Yes")) == 0) 
		    dlr = 1;
	       else 
		    dlr = 0;

	       qf = settings->qfs->mms_queue_add(xfrom, lto, NULL, xproxy, NULL,
						 0, time(NULL) + settings->default_msgexpiry, msg, NULL, 
						 NULL, NULL,
						 NULL, NULL,
						 NULL,
						 dlr,
						 octstr_get_cstr(settings->global_queuedir), 
						 mm4_type ? "MM4" : "MM3",
						 settings->host_alias);
	       if (qf) {
		    newmsgid = mms_make_msgid(octstr_get_cstr(qf), 
					      settings->host_alias);		  
		    mms_info(0, "mmsfromemail", NULL, "%s Queued message to %s from %s (via %s) => %s",
			 mm4_type ? "MM4 Incoming" : "Email2MMS",
			 octstr_get_cstr(xto), octstr_get_cstr(xfrom),
			 xproxy ? octstr_get_cstr(xproxy) : "(None)", octstr_get_cstr(qf));
		    octstr_destroy(qf);

		    /* Queue our response to the chap. */
		    err = "Ok";		    
	       } else
		    err = "Error-network-problem";
	       mms_log("Received", xfrom, lto, 
		       -1, msgid, NULL, xproxy, mm4_type ? "MM4" : "MM3", NULL,NULL);
	       
	       gwlist_destroy(lto,NULL);
	       if (dreport) 
		    octstr_destroy(dreport);
	       
	  }
	  break;
     case MM4_DELIVERY_REPORT_REQ: 
     	  if (ttype != TPLMN  && settings->mms2email == NULL) { /* We only send to phones from this interface */
	       mms_error(0, "MM4", NULL, "Not allowed to send to %s!", octstr_get_cstr(xto));
	       err = "Error-service-denied";
	  } else {
	       List *lto = gwlist_create();
	       Octstr *qf;

	       if (ttype == TPLMN)	       
		    octstr_format_append(xto, "/TYPE=PLMN");
	       else 
		    octstr_format_append(xto, "@%S", rhost);
	       gwlist_append(lto, xto);
	       qf = settings->qfs->mms_queue_add(xfrom, lto, NULL,
						 xproxy, NULL,
						 0, time(NULL) + settings->default_msgexpiry, msg, NULL,
						 NULL, NULL,
						 NULL, NULL,
						 NULL,
						 0,
						 octstr_get_cstr(settings->global_queuedir), 
						 "MM4",
						 settings->host_alias);
	       gwlist_destroy(lto, NULL);
	       if (qf) {
 		    mms_info(0, "MM4", xproxy,"Queued DLR from proxy %s to %s from %s => %s",
			 octstr_get_cstr(xproxy), octstr_get_cstr(xto), octstr_get_cstr(xfrom),
			 octstr_get_cstr(qf));		    
		    octstr_destroy(qf);
		    err = "Ok";
	       } else 
		    err = "Error-network-problem";
	       newmsgid = msgid ? octstr_duplicate(msgid) : NULL; /* report old msg id */
	  }     
	  break;

     case MM4_READ_REPLY_REPORT_REQ:
	  if (ttype != TPLMN   && settings->mms2email == NULL)  { /* We only send to phones from this interface */
	       mms_error(0, "MM4", NULL, "Not allowed to send to %s!", octstr_get_cstr(xto));
	       err = "Error-service-denied";
	  } else {
	       List *lto = gwlist_create();
	       Octstr *qf;
	       
	       if (ttype == TPLMN)	       
		    octstr_format_append(xto, "/TYPE=PLMN");
	       else 
		    octstr_format_append(xto, "@%S", rhost);

	       gwlist_append(lto, xto);
	       qf = settings->qfs->mms_queue_add(xfrom, lto, NULL,
						 xproxy, NULL,
						 0, time(NULL) + settings->default_msgexpiry, msg, NULL,
						 NULL, NULL,
						 NULL, NULL,
						 NULL,
						 0,
						 octstr_get_cstr(settings->global_queuedir), 
						 "MM4",
						 settings->host_alias);
	       gwlist_destroy(lto, NULL);
	       if (qf) {
 		    mms_info(0, "MM4", xproxy, "Queued read report from proxy %s to %s from %s => %s",
			 octstr_get_cstr(xproxy), octstr_get_cstr(xto), octstr_get_cstr(xfrom),
			 octstr_get_cstr(qf));		    
		    octstr_destroy(qf);
		    err = "Ok";
	       } else 
		    err = "Error-network-problem";
	  }
	  break;
     case MM4_FORWARD_RES:
     case MM4_READ_REPLY_REPORT_RES:
     case MM4_DELIVERY_REPORT_RES: /* remove corresponding queue entry. */
     {
	  Octstr *qf, *o_to;
	  int i;
	  
	  /* Pull the number out of the fake transaction ID */
	  if ((i = octstr_search_char(transid, '-',0)) > 0) {
	       o_to = octstr_copy(transid, 0, i);
#if 0
	       _mms_fixup_address(o_to, settings->unified_prefix ? octstr_get_cstr(settings->unified_prefix) : NULL); 
#else
	       mms_normalize_phonenum(&o_to, 
				      octstr_get_cstr(settings->unified_prefix), 
				      settings->strip_prefixes);
	       
#endif
	       octstr_delete(transid, 0, i+1);
	  } else
	       o_to = NULL;
	  qf = mms_getqf_fromtransid(transid);	  	  
	  if (qf) {
	       MmsEnvelope *e;
	       octstr_strip_blanks(qf);
	       strip_quotes(qf);
	       octstr_strip_blanks(o_to);
	       strip_quotes(o_to);
	       e = settings->qfs->mms_queue_readenvelope(octstr_get_cstr(qf),
						       octstr_get_cstr(settings->global_queuedir),
						       1);
	       if (!e) 
		    mms_warning(0, "MM4", xproxy, "MM4 Received %s from %s but cannot find queue entry for transaction %s [%s]!", 
			    mm4_types[mtype].mm4str,
			    octstr_get_cstr(xproxy),
			    octstr_get_cstr(transid), 
			    octstr_get_cstr(qf));
	       else {
		    MmsEnvelopeTo *t;
		    int i, n;
		    int processed = 0;
		    
		    for (i = 0, n = gwlist_len(e->to); i<n; i++)
			 if ((t = gwlist_get(e->to, i)) != NULL && 
			     (o_to == NULL || octstr_case_compare(o_to, t->rcpt) == 0)) {
			      t->process = 0; /* Should make it go away. */
			      processed = 1;
			 }
		    /* write CDR if it is a forward confirmation */
		    if (processed && mtype == MM4_FORWARD_RES && 
			rstatus && octstr_str_case_compare(rstatus, "Ok") == 0) {
			 MmsCdrStruct *cdr = make_cdr_struct(settings->mms_bill_module_data, 
							     e->created, octstr_get_cstr(e->from), 
							     o_to ? octstr_get_cstr(o_to) : "",
							     e->msgId ? octstr_get_cstr(e->msgId) : "",
							     e->vaspid ? octstr_get_cstr(e->vaspid) : "",
							     e->src_interface,
							     "MM4", e->msize);			 
			 settings->mms_billfuncs->mms_logcdr(cdr);
			 
			 gw_free(cdr);
		    }
		    
		    mms_info(0, "MM4", xproxy, "Received %s from proxy %s to %s from %s => %s, status: [%s, %s]",
			 mm4_types[mtype].mm4str,
			 octstr_get_cstr(xproxy), o_to ? octstr_get_cstr(o_to) : octstr_get_cstr(xto),  
			 octstr_get_cstr(xfrom),
			 octstr_get_cstr(qf),
			 rstatus ? octstr_get_cstr(rstatus) : "",
			 processed ? "Sender number matched in queue file" : "Sender number not matched in queue file");
		    
		    if (settings->qfs->mms_queue_update(e) != 1)
			 settings->qfs->mms_queue_free_env(e);
	       }
	       
	  } else 
	       mms_warning(0, "MM4", xproxy, "Received %s but cannot find message %s in queue!", 
		       mm4_types[mtype].mm4str,
		       octstr_get_cstr(transid)); 

	  octstr_destroy(o_to);
	  
     }
     break;
     default: 
	  mms_warning(0, "MM4", xproxy, "Unexpected message type: %s", 
		  mm4_type  ? octstr_get_cstr(mm4_type) : "not given!");
	  break;
     }
     
     /* respond to the sender as necessary. */
     if (mm4_type && 
	 err && 
	 ack && octstr_str_case_compare(ack, "Yes") == 0) {
	  int i, n;
	  Octstr *sendmail_cmd = settings->sendmail;
	  /* try and find proxy and it's send command. */
	  if (xproxy)
	       for (i = 0,  n = gwlist_len(proxyrelays); i<n; i++) {
		    MmsProxyRelay *mp = gwlist_get(proxyrelays, i);
		    if (mp && octstr_case_compare(xproxy, mp->host) == 0 && 
			mp->sendmail) {
			 sendmail_cmd = mp->sendmail;
			 break;
		    }
	       }
	  send_mm4_res(mtype+1, orig_sys, me, transid, err, newmsgid, sendmail_cmd);     
     }
     octstr_destroy(mm4_type);     
     octstr_destroy(transid);     
     octstr_destroy(orig_sys);
     octstr_destroy(msgid);
     octstr_destroy(newmsgid);          
     octstr_destroy(rstatus);
     octstr_destroy(xto);     
     octstr_destroy(xfrom);
     octstr_destroy(xproxy);
     octstr_destroy(me);

     octstr_destroy(rhost);

     mms_destroy(msg);
     mms_cleanup_mmsc_settings(settings);
     mms_lib_shutdown();

     return 0;
     
}


static int find_own(int i, int argc, char *argv[])
{
     if (argv[i][1] == 'f')
	  if (i + 1  < argc) {
	       xfrom = octstr_create(argv[i+1]);
	       return 1;
	  } else
	       return -1;
     else if (argv[i][1] == 't') /* recipient. */
	  if (i + 1  < argc) {
	       xto = octstr_create(argv[i+1]);
	       return 1;
	  } else
	       return -1;
     else if (argv[i][1] == 's') /* Proxy name if any. */
	  if (i + 1  < argc) {
	       xproxy = octstr_create(argv[i+1]);
	       return 1;
	  } else
	       return -1;
     else if (argv[i][1] == 'n') {
	  no_strip = 1;
	  return 0;
     } else if (argv[i][1] == 'x') {
	  strip_type = 1;
	  return 0;
     } else 
	  return -1;
}


static void fixup_recipient(Octstr **host)
{
     int i;
     Octstr *typ = NULL;
   

     if (!xto) return;

     i = octstr_search_char(xto, '@', 0);     /* Remove '@' */
     if (i>0) {
	  *host = octstr_copy(xto, i+1, octstr_len(xto));
	  octstr_delete(xto, i, octstr_len(xto));
     } else 
	  *host = octstr_create("localhost");

     i = octstr_search(xto, octstr_imm("/TYPE="), 0);
     if (i > 0) {
	  typ = octstr_copy(xto, i+1, octstr_len(xto));
	  octstr_delete(xto, i,  octstr_len(xto));
     }
     /* XXX may be we should use fixup function in mmlib/mms_util.c ?? */
     if (isphonenum(xto) && 
	 (!typ || octstr_str_compare(typ, "TYPE=PLMN") == 0)) { /* A phone number. */
#if 0
	  normalize_number(octstr_get_cstr(settings->unified_prefix), &xto);     
#else

	  mms_normalize_phonenum(&xto, 
				 octstr_get_cstr(settings->unified_prefix), 
				 settings->strip_prefixes);
#endif
	  ttype = TPLMN;
     } else { /* For now everything else is email. */
	  ttype = TEMAIL;
     }

     octstr_destroy(typ);
}

static void fixup_sender(void)
{
     int i, isphone = 1;
     
     /* Find the TYPE=xxx element. If it is there, it is a number. Strip the @ */

     if (!xfrom) return;
     
     i = octstr_case_search(xfrom, octstr_imm("/TYPE="), 0);
     if (i>0) {
	  int j = octstr_search_char(xfrom, '@', 0);
	  if (j > i) { /* we have @, remove it */
	       if (xproxy == NULL)
		    xproxy = octstr_copy(xfrom, j+1, octstr_len(xfrom));
	       octstr_delete(xfrom, j, octstr_len(xfrom));
	  }	      
	  if (strip_type) 
	       octstr_delete(xfrom, i, octstr_len(xfrom));
     } else if (isphonenum(xfrom)) { /* Add the TYPE element if missing. */
	  if (!strip_type)
	       octstr_append(xfrom, octstr_imm("/TYPE=PLMN"));     
     } else {
	  i = octstr_search_char(xfrom, '@', 0);     
	  if (i<0) 
	       octstr_format_append(xfrom, "@unknown");
	  else if (xproxy == NULL)
	       xproxy = octstr_copy(xfrom, i+1, octstr_len(xfrom));
	  isphone = 0;
     }
     /* clean the number. */
     _mms_fixup_address(&xfrom, 
			octstr_get_cstr(settings->unified_prefix), 
			settings->strip_prefixes, strip_type ? 0 : 1);     	       
     
     if (no_strip && isphone && xproxy) 
	  octstr_format_append(xfrom, "@%S", xproxy);
}

static void fixup_address_type(List *headers, char *hdr)
{
     List *l;
     int i, n;
     
     l = http_header_find_all(headers, hdr);
     
     http_header_remove_all(headers,hdr);
     for (i = 0, n = gwlist_len(l); i<n; i++) {
	  Octstr *name, *value;
	  int j, k;
	  http_header_get(l, i, &name, &value);
	  
	  if (!value || !name ||	 
	      octstr_case_compare(name, octstr_imm(hdr)) != 0)	 
	       goto loop;
	  if ((j = octstr_search(value, octstr_imm("/TYPE="), 0))>0) {
	       k = octstr_search_char(value, '@', 0);
	       if (k > j) { /* we have @, after TYPE=PLMN remove it */
		    octstr_delete(value, k, octstr_len(value));
	       }
	  }
	  _mms_fixup_address(&value, 
			     octstr_get_cstr(settings->unified_prefix), 
			     settings->strip_prefixes, 1);     	       
	  
	  http_header_add(headers, octstr_get_cstr(name), octstr_get_cstr(value));
     loop:	 
	  octstr_destroy(value);	 
	  octstr_destroy(name);	 	  
     }
     http_destroy_headers(l);     
}

static void fixup_addresses(List *headers)
{
     fixup_address_type(headers, "To");
     fixup_address_type(headers, "From");
}

static void send_mm4_res(int mtype, Octstr *to, Octstr *sender, Octstr *transid, char *status, Octstr *msgid, 
     Octstr *sendmail_cmd)
{
     char tmp[32];
     List *h = http_create_empty_headers();
     MIMEEntity *m = mime_entity_create();
     Octstr *err  = NULL;
     /* Make headers */
     sprintf(tmp, "%d.%d.%d", 
	     MAJOR_VERSION(MMS_3GPP_VERSION),
	     MINOR1_VERSION(MMS_3GPP_VERSION),
	     MINOR2_VERSION(MMS_3GPP_VERSION));
     
     http_header_add(h, "X-Mms-3GPP-MMS-Version", tmp);
     http_header_add(h, "X-Mms-Transaction-ID", octstr_get_cstr(transid));
     http_header_add(h, "X-Mms-Message-Type", mm4_types[mtype].mm4str);
     if (msgid) 
	  http_header_add(h, "X-Mms-Message-ID", octstr_get_cstr(msgid));	  
     http_header_add(h, "X-Mms-Request-Status-Code", status);
     http_header_add(h, "Sender", octstr_get_cstr(sender));     
     http_header_add(h, "To", octstr_get_cstr(to));     
     
     mime_replace_headers(m, h);
     http_destroy_headers(h);
     
     mm_send_to_email(to, sender, octstr_imm(""), msgid, m, 0, &err, octstr_get_cstr(sendmail_cmd),
		      settings->hostname);
     if (err) {
	  mms_warning(0, "MM4", NULL, "send.RES reported: %s!", octstr_get_cstr(err));
	  octstr_destroy(err);
     }
     mime_entity_destroy(m);
}

static void strip_quotes(Octstr *s)
{
     int l = s ? octstr_len(s) : 0;

     if (l == 0)
     	return;
     if (octstr_get_char(s, 0) == '"') {
     	octstr_delete(s, 0, 1);
     	l--;
     }
     if (octstr_get_char(s, l-1) == '"')
     	octstr_delete(s, l-1, 1);
}


/*
 * Mbuni - Open  Source MMS Gateway
 *
 * Mbuni MmsBox MM1 Component - Send & Receive MMS using a GPRS/EDGE/3G modem
 *
 * Copyright (C) 2007-2009, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include "mmsbox_mmsc.h"
#include "mmsbox_cfg.h"
#include "mmsbox.h"
#include "mms_util.h"

typedef struct {
     Octstr *mmsc_url; /* mmsc-url. */
     Octstr *msisdn;   /* my msisdn. */
     Octstr *proxy;    /* form is host:port. */
     Octstr *gprs_on;  /* Command to start GPRS link. Must not exit. */
     Octstr *gprs_pid; /* command to call to get PID of GPRS for stopping GPRS link (i.e. pppd). */
     Octstr *smsc_on;  /* command to start smsc connection */
     Octstr *smsc_off; /* commadn to stop smsc connection  */
     int    port;      /* port for incoming messages. */
     Octstr *interface; /* specific interface to listen on. */

     Octstr *unified_prefix; /* copies from above, do not edit! */
     List *strip_prefixes;

     /* internal data. */
     MmscGrp *info;
     MmsQueueHandlerFuncs *qfs;
     List *requests;   /* list of requests. */
     long h_tid, d_tid;         /* thread IDs.  */

     int sender_alive;
} MM1Settings;

typedef struct {
     enum {MM1_GET, MM1_PUSH} type;
     int waiter_exists;     /* set to true if after handling, should signal and NOT destroy struct.*/
     pthread_cond_t   cond;
     pthread_mutex_t mutex;
     union {
	  MmsMsg *m;   /* for push. */
	  Octstr *url;  /* for get   */
     } u;
     void *result;  /* set to the result for a PUSH */
     Octstr *error;
} MM1Request;

static void handle_notify(MM1Settings *mm1);
static void handle_mm1(MM1Settings *mm1);
static int open_mm1(MmscGrp *mmc, MmsQueueHandlerFuncs *qfs,
		    Octstr *unified_prefix, List *strip_prefixes,
		    void **data)
{
     Octstr *x, *y;
     List *l;
     Octstr *mmsc_url = NULL;
     Octstr *proxy = NULL;
     Octstr *gprs_on = NULL;
     Octstr *gprs_pid = NULL;
     Octstr *smsc_on = NULL;
     Octstr *smsc_off = NULL;
     int  port = 80;
     Octstr *interface = NULL;
     Octstr *msisdn = NULL;

     MM1Settings *mm1;

     /* parse the settings. */

     x = octstr_duplicate(mmc->settings);
     l = octstr_split(x, octstr_imm(";"));
     octstr_destroy(x);
     if (l)
	  while ((y = gwlist_extract_first(l)) != NULL) {
	       int i = octstr_search_char(y, '=', 0);
	       Octstr *item = (i>=0) ? octstr_copy(y, 0, i) : NULL;
	       Octstr *value = (i>=0) ? octstr_copy(y, i+1, octstr_len(y)) : NULL;

	       if (item == NULL)
		    octstr_destroy(value);
	       else if (octstr_str_case_compare(item, "mmsc-url") == 0)
		    mmsc_url = value;
	       else if (octstr_str_case_compare(item, "proxy") == 0)
		    proxy = value;
	       else if (octstr_str_case_compare(item, "gprs-on") == 0)
		    gprs_on = value;
	       else if (octstr_str_case_compare(item, "gprs-pid") == 0)
		    gprs_pid = value;
	       else if (octstr_str_case_compare(item, "port") == 0) {
		    port = value ? atoi(octstr_get_cstr(value)) : 80;
		    if (http_open_port(port, 0) != 0)
			 mms_warning(0, "mmsbox-mm1",  NULL, "failed to open incoming notifications port %d: %s",
			       port, strerror(errno));
	       }else if (octstr_str_case_compare(item, "http-interface") == 0)
		    interface = value;
	       else if (octstr_str_case_compare(item, "smsc-on") == 0)
		    smsc_on = value;
	       else if (octstr_str_case_compare(item, "smsc-off") == 0)
		    smsc_off = value;
	       else if (octstr_str_case_compare(item, "msisdn") == 0)
		    msisdn = value;
	       else {
		    mms_error(0, "mmsbox-mm1", NULL, "unknown/unsupported option: %s", octstr_get_cstr(item));
		    octstr_destroy(value);
	       }
	       octstr_destroy(item);
	       octstr_destroy(y);
	  }
     else {
	  mms_error(0, "mmsbox-mm1", NULL, "failed to parse settings string: %s", octstr_get_cstr(mmc->settings));
	  return -1;
     }

     gwlist_destroy(l, NULL);

     mm1 = gw_malloc(sizeof *mm1);
     mm1->mmsc_url = mmsc_url;
     mm1->proxy = proxy;
     mm1->gprs_on = gprs_on;
     mm1->gprs_pid = gprs_pid;
     mm1->smsc_on = smsc_on;
     mm1->smsc_off = smsc_off;
     mm1->port = port;
     mm1->interface = interface;
     mm1->msisdn = msisdn;

     mm1->unified_prefix = unified_prefix;
     mm1->strip_prefixes = strip_prefixes;

     mm1->qfs = qfs;
     mm1->info = mmc;
     mm1->requests = gwlist_create();
     mm1->sender_alive = 0;
     gwlist_add_producer(mm1->requests); /* so that when first listener starts, knows there is a consumer. */
     mm1->h_tid = gwthread_create((gwthread_func_t *)handle_notify, mm1);
     mm1->d_tid = gwthread_create((gwthread_func_t *)handle_mm1, mm1);

     *data = mm1;
     return 0;
}


static void handle_notify(MM1Settings *mm1)
{
     HTTPClient *client = NULL;
     Octstr *ip = NULL, *url = NULL;
     Octstr *body = NULL;
     List *cgivars = NULL, *h = NULL;

     while ((client = http_accept_request(mm1->port, &ip, &url, &h, &body, &cgivars)) != NULL) {
	  List *cgivar_ctypes = NULL;
	  Octstr *text, *rb = NULL, *s = NULL, *loc = NULL;
	  MmsMsg *m = NULL;
	  int hdrlen, status = HTTP_ACCEPTED, mtype;
	  List *mh = NULL, *to = gwlist_create(), *rh = http_create_empty_headers();
	  time_t expiryt = -1, deliveryt = -1;
	  Octstr *from = NULL, *subject = NULL, *otransid = NULL, *mmc_id = NULL;
	  Octstr *qdir;



	  parse_cgivars(h, body, &cgivars, &cgivar_ctypes);
	  text = http_cgi_variable(cgivars, "text");

	  if (!text) {
	       rb = octstr_imm("mmsbox-mm1: missing 'text' CGI parameter!");
	       status = HTTP_NOT_FOUND;
	       goto loop;
	  }
	  /* now parse the MMS, determine what kind it is,
	     queue the fetch request or deal with it directly.
	  */
	  hdrlen = octstr_get_char(text, 2);
	  if ((s = octstr_copy(text, 3 + hdrlen, octstr_len(text))) != NULL)
	       m = mms_frombinary(s, mm1->msisdn);
	  else
	       m = NULL;

	  if (m == NULL) {
	       rb = octstr_imm("mmsbox-mm1: mal-formed mms packet on interface!");
	       status = HTTP_FORBIDDEN;
	       goto loop;
	  } else
	       mms_msgdump(m, 1);

	  /* rest of this copied largely from EAIF code. */
	  mh = mms_message_headers(m);
	  mtype = mms_messagetype(m);
	  mms_collect_envdata_from_msgheaders(mh, &to, &subject,
					      &otransid, &expiryt, &deliveryt,
					      DEFAULT_EXPIRE, -1,
					      octstr_get_cstr(mm1->unified_prefix),
					      mm1->strip_prefixes);
	  from = http_header_value(mh, octstr_imm("From"));

	  qdir = get_mmsbox_queue_dir(from, to, mm1->info, &mmc_id); /* get routing info. */
	  switch (mtype) {
	       Octstr *qf;
	       Octstr *dlr_url, *status_value, *msgid;
	       List *rqh;
	  case MMS_MSGTYPE_DELIVERY_IND: /* notification of a delivery. */
	  case MMS_MSGTYPE_READ_ORIG_IND: /* message read. */
	       msgid = http_header_value(mh, octstr_imm("Message-ID"));
	       status_value = http_header_value(mh,
						(mtype == MMS_MSGTYPE_DELIVERY_IND) ?
						octstr_imm("X-Mms-Status") :
						octstr_imm("X-Mms-Read-Status"));

	       rqh = http_create_empty_headers();

	       dlr_url = mmsbox_get_report_info(m, mm1->info, mmc_id,
						(mtype == MMS_MSGTYPE_DELIVERY_IND) ?
						"delivery-report" : "read-report",
                                                status_value, rqh, NULL, 0, msgid);

	       qf = mm1->qfs->mms_queue_add(from, to, NULL,
					    mm1->info->id, mmc_id,
					    0, time(NULL) + default_msgexpiry, m, NULL,
					    NULL, NULL,
					    dlr_url, NULL,
					    rqh,
					    0,
					    octstr_get_cstr(qdir),
					    "MM7/MM1-IN",
					    octstr_imm(MM_NAME));
	       if (qf)
		    /* Log to access log */
		    mms_log((mtype == MMS_MSGTYPE_DELIVERY_IND) ? "Received DLR" : "Received RR",
			    from, to, -1, msgid, status_value, mm1->info->id,
			    "MMSBox", octstr_imm("MM1"), NULL);
	       else
		    status = HTTP_INTERNAL_SERVER_ERROR;
	       octstr_destroy(qf);
	       octstr_destroy(msgid);
	       octstr_destroy(dlr_url);
	       octstr_destroy(status_value);
	       http_destroy_headers(rqh);
	       break;

	  case MMS_MSGTYPE_NOTIFICATION_IND: /* notification of an incoming message. */
	       if ((loc = http_header_value(mh, octstr_imm("X-Mms-Content-Location"))) != NULL) {
		    MM1Request *r = gw_malloc(sizeof *r);

		    memset(r, 0, sizeof *r);
		    r->type = MM1_GET;
		    r->u.url = loc;
		    r->waiter_exists = 0;
		    loc = NULL;

		    gwlist_produce(mm1->requests, r);
	       } else
		    rb = octstr_format("mmsbox-mm1: notification with content-location??");
	       break;
	  default:
	       rb = octstr_format("mmsbox-mm1: unexpected message type: %s",
				  mms_message_type_to_cstr(mtype));
	       status = HTTP_NOT_FOUND;
	       break;
	  }


     loop:
	  /* send reply. */
	  http_header_add(rh, "Content-Type", "text/plain");
	  http_send_reply(client, status, rh, rb ? rb : octstr_imm(""));

	  octstr_destroy(s);
	  octstr_destroy(ip);
	  octstr_destroy(url);
	  octstr_destroy(loc);
	  octstr_destroy(mmc_id);
	  octstr_destroy(from);
	  octstr_destroy(subject);
	  octstr_destroy(otransid);
	  octstr_destroy(body);
	  octstr_destroy(rb);

	  gwlist_destroy(to, (void *)octstr_destroy);
	  http_destroy_headers(h);
	  http_destroy_headers(rh);
	  http_destroy_headers(mh);
	  http_destroy_cgiargs(cgivars);
	  http_destroy_cgiargs(cgivar_ctypes);
	  mms_destroy(m);
     }

     mms_info(0, "mmsbox-mm1", NULL, "handle_notify exits");
     gwlist_remove_producer(mm1->requests); /* cause consumers to shut down. */
}


static Octstr *send_msg(void *data, Octstr *from, Octstr *to,
			 Octstr *transid,
			 Octstr *linkedid, char *vasid, Octstr *service_code,
			 MmsMsg *m, List *hdrs, Octstr **err, int *retry)
{
     MM1Request *r = gw_malloc(sizeof *r);
     MM1Settings *s = data;
     Octstr *id;

     gw_assert(data);
     gw_assert(m);

     if (!s->sender_alive) {
	  *err =  octstr_imm("internal error, mm1 notify not started!");
	  *retry  = 1;
	  return NULL;
     }

     /* Remove the from address first of all, replace the to address as well */
     mms_replace_header_value(m, "From", "#insert");
     mms_replace_header_value(m, "To", octstr_get_cstr(to));
     mms_remove_headers(m, "Message-ID");

     r->u.m = m;
     pthread_cond_init(&r->cond, NULL);
     pthread_mutex_init(&r->mutex, NULL);
     r->waiter_exists = 1;
     r->type = MM1_PUSH;
     r->result = NULL;
     r->error = NULL;

     pthread_mutex_lock(&r->mutex); /* at pickup, must grab mutex before signalling. otherwise race condition.*/

     gwlist_produce(s->requests, r);

     pthread_cond_wait(&r->cond, &r->mutex);

     *err = r->error;

     id = r->result;
     mms_info(0, "mmsbox-mm1", NULL, "sent message, type=%s, result=%s",
	  mms_message_type_to_cstr(mms_messagetype(m)), r->error ? octstr_get_cstr(r->error) : "(none)");
     /* destroy the structure. */

     pthread_cond_destroy(&r->cond);
     pthread_mutex_destroy(&r->mutex);
     gw_free(r);
     *retry = 1; /*  always retry ?? */

     return id;
}

static long start_gprs(Octstr *cmd, Octstr *pid_cmd);
static Octstr *fetch_content(Octstr *url, Octstr *proxy, Octstr *body, int *hstatus);
static void handle_mm1(MM1Settings *mm1)
{
     /* stop smsc, start GPRS, transact, stop GPRS, start SMSC. And so on. */
     MM1Request *r;

     mms_info(0, "mmsbox-mm1", NULL, "handle_mm1 started");
     mm1->sender_alive++;
     while ((r = gwlist_consume(mm1->requests)) != NULL) {
	  long n, pid = -1;
	  if (mm1->smsc_off) {
	       n = system(octstr_get_cstr(mm1->smsc_off));
	       gwthread_sleep(5); /* allow it to die. */
	  }
	  if (mm1->gprs_on)
	       pid = start_gprs(mm1->gprs_on, mm1->gprs_pid);

	  if (pid  < 0) {
	       mms_warning(0, "mmsbox-mm1", NULL,"failed to start GPRS connection. waiting...");
	       gwthread_sleep(2);
	       goto kill_gprs;
	  } else
	       mms_info(0, "mmsbox-mm1", NULL, "start_gprs returned PID: %ld", pid);

	  do {
	       Octstr *body;
	       Octstr *url;
	       int hstatus  = 0;
	       Octstr *ms;
	       MmsMsg *m;
	       int msize;

	       if (r->waiter_exists)
		    pthread_mutex_lock(&r->mutex); /* grab lock to avoid race condition */

	       body = (r->type == MM1_PUSH) ? mms_tobinary(r->u.m) : NULL;
	       url  = (r->type == MM1_PUSH) ? mm1->mmsc_url : r->u.url;
	       ms   = fetch_content(url, mm1->proxy, body, &hstatus);
	       msize = ms ? octstr_len(ms) : 0;
	       m    = (hstatus == 0 && ms)  ? mms_frombinary(ms, mm1->msisdn) : NULL;

	       if (r->type == MM1_GET) {
		    if (m == NULL)
			 mms_error(0, "mmsbox-mm1", NULL, "failed to fetch mms from URL: %s!",
			       octstr_get_cstr(url));
		    else {
			 List *mh = mms_message_headers(m), *to = gwlist_create();
			 Octstr *subject = NULL, *otransid = NULL, *msgid = NULL, *value;
			 Octstr *hfrom = mh ? http_header_value(mh, octstr_imm("From")) : octstr_imm("anon@anon");
			 Octstr *qf = NULL, *qdir = NULL, *mmc_id = NULL;
			 time_t expiryt = -1, deliveryt = -1;
			 int dlr;

			 /* we assume it is a true message (send_req|retrieve_conf) */
			 mms_collect_envdata_from_msgheaders(mh, &to, &subject,
							     &otransid, &expiryt, &deliveryt,
							     DEFAULT_EXPIRE, -1,
							     octstr_get_cstr(mm1->unified_prefix),
							     mm1->strip_prefixes);

			 msgid = http_header_value(mh, octstr_imm("Message-ID"));
			 value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"));
			 if (value &&
			     octstr_case_compare(value, octstr_imm("Yes")) == 0)
			      dlr = 1;
			 else
			      dlr = 0;
			 octstr_destroy(value);

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

			 qdir = get_mmsbox_queue_dir(hfrom, to, mm1->info, &mmc_id); /* get routing info. */
			 /* Save it,  put message id in header, return. */
			 qf = qfs->mms_queue_add(hfrom, to, subject,
						 mm1->info->id, mmc_id,
						 deliveryt, expiryt, m, NULL,
						 NULL, NULL,
						 NULL, NULL,
						 NULL,
						 dlr,
						 octstr_get_cstr(qdir),
						 "MM7/MM1-IN",
						 octstr_imm(MM_NAME));

			 if (qf)
			      /* Log to access log */
			      mms_log("Received", hfrom, to, msize,
				      msgid, NULL, mm1->info->id, "MMSBox",octstr_imm("MM1"), NULL);
			 else
			      mms_error(0, "mmsbox-mm1", NULL, "failed to create queue entry for URL %s",
				    octstr_get_cstr(url));

			 if (otransid) { /* tell mmsc that we fetched fine. */
			      int _status;
			      MmsMsg *mresp = mms_notifyresp_ind(octstr_get_cstr(otransid),
								 mms_message_enc(m), "Retrieved", 1);
			      Octstr *sm = mms_tobinary(mresp);
			      Octstr *_x = fetch_content(mm1->mmsc_url, mm1->proxy,
							 sm, &_status);

			      octstr_destroy(_x);
			      octstr_destroy(sm);
			      mms_destroy(mresp);
			 }
			 gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
			 octstr_destroy(hfrom);
			 octstr_destroy(subject);
			 octstr_destroy(otransid);
			 octstr_destroy(msgid);
			 octstr_destroy(qf);
			 octstr_destroy(mmc_id);

			 http_destroy_headers(mh);
		    }

	       }  else if (r->type ==  MM1_PUSH) {
		    /* we expect a send-conf. */
		    if (ms) {
			 octstr_dump(ms, 0);

			 mms_msgdump(m, 1);
		    } else
			 mms_warning(0, "mmsbox-mm1", NULL,"No send-conf returned by operator");

		    if (m == NULL ||
			(r->result = mms_get_header_value(m, octstr_imm("Message-ID"))) == NULL) {
			 Octstr *err = m ? mms_get_header_value(m, octstr_imm("X-Mms-Response-Text")) : NULL;
			 Octstr *status = m ? mms_get_header_value(m, octstr_imm("X-Mms-Response-Status")) : NULL;
			 mms_error(0, "mmsbox-mm1", NULL, "Sending failed: %s, %s!",
			       err ? octstr_get_cstr(err) : "(none)",
			       status ? octstr_get_cstr(status) : "(none)");
			 octstr_destroy(err);
			 octstr_destroy(status);
		    }
	       } else
		    mms_error(0, "mmsbox-mm1", NULL, "unknown type: %d", r->type);

	       if (r->waiter_exists) {
		    pthread_mutex_unlock(&r->mutex);
		    pthread_cond_signal(&r->cond);
	       } else  /* no waiter, so we free it ourselves. */
		    gw_free(r);

	       octstr_destroy(body);
	       octstr_destroy(ms);
	       mms_destroy(m);
	       r = NULL;
	       pid_t wp;
	       int st;
	       wp = waitpid(pid, &st, WNOHANG);
	       if(wp == pid && WIFEXITED(st)) {
		 mms_info(0, "mmsbox-mm1", NULL, "GPRS pid (%d) appears to be dead - quitting loop", pid);
		 goto after_gprs_dead;
	       }
	       gwthread_sleep(2); /* according to Piotr Isajew, this makes life better */
	  } while (gwlist_len(mm1->requests) > 0 &&
		   (r = gwlist_consume(mm1->requests)) != NULL);

     kill_gprs:
	  if(r != NULL) {
	    if(r->waiter_exists) {
	      pthread_mutex_unlock(&r->mutex);
	      pthread_cond_signal(&r->cond);
	    } else{
	      gw_free(r);
	    }


	  }
	  if (pid > 0) { /* stop GPRS, restart SMSC connection. */
	       int xkill, status;
	       pid_t wpid;
	       do {
		    xkill = kill(pid, SIGTERM);
		    mms_info(0, "mmsbox-mm1", NULL, "GPRS turned off returned: %d", xkill);
		    if (xkill < 0 && errno == ESRCH)
			 break;
		    wpid = waitpid(pid, &status, 0);
		    if (wpid == pid && WIFEXITED(status))
			 break;
		    else if (wpid < 0 && errno == ECHILD)
			 break;
	       } while (1);
	       gwthread_sleep(2);
	  }
     after_gprs_dead:
	  if (mm1->smsc_on) {
	       system(octstr_get_cstr(mm1->smsc_on));
	       gwthread_sleep(5);
	       mms_info(0, "mmsbox-mm1", NULL, "SMSC turned on");
	  }

     }
     mm1->sender_alive--;
     mms_info(0, "mmsbox-mm1", NULL, "handle_mm1 exits");
}

static int close_mm1(void *data)
{
     MM1Settings *s = data;

     /* close the port, and all will die. */
     http_close_port(s->port);
     gwthread_sleep(2);
     gwthread_join(s->h_tid);
     gwthread_sleep(2);
     gwthread_join(s->d_tid);

     octstr_destroy(s->mmsc_url);
     octstr_destroy(s->msisdn);
     octstr_destroy(s->proxy);
     octstr_destroy(s->gprs_on);
     octstr_destroy(s->gprs_pid);
     octstr_destroy(s->smsc_on);
     octstr_destroy(s->smsc_off);
     octstr_destroy(s->interface);

     gwlist_destroy(s->requests, NULL);

     gw_free(s);

     return 0;
}

MmsBoxMmscFuncs mmsc_funcs = {
     open_mm1,
     close_mm1,
     send_msg
};


#include <curl/curl.h>


static int write_octstr_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
     Octstr *out = userp;

     octstr_append_data(out, buffer, size*nmemb);
     mms_info(0, "mmsbox-mm1", NULL,  "write_data called with nmemn=%ld, size=%ld", nmemb, size);
     return size*nmemb;
}

static Octstr *fetch_content(Octstr *url, Octstr *proxy, Octstr *body, int *hstatus)
{
     CURL *cl;
     struct curl_slist *h = NULL;
     char errbuf[512];
     static int curl_inited = 0;
     Octstr *s = octstr_create("");

     if (curl_inited == 0) {
	  curl_global_init(CURL_GLOBAL_ALL);
	  curl_inited = 1;
     }

     cl = curl_easy_init();
     curl_easy_setopt(cl, CURLOPT_URL, octstr_get_cstr(url));
     if (proxy && octstr_len(proxy) > 0)
	  curl_easy_setopt(cl, CURLOPT_PROXY, octstr_get_cstr(proxy));
     curl_easy_setopt(cl, CURLOPT_WRITEFUNCTION, write_octstr_data);
     curl_easy_setopt(cl, CURLOPT_WRITEDATA, s);
     curl_easy_setopt(cl, CURLOPT_NOSIGNAL, 1L);
     curl_easy_setopt(cl, CURLOPT_TIMEOUT, 120L);
     curl_easy_setopt(cl, CURLOPT_FORBID_REUSE, 1L);
     curl_easy_setopt(cl, CURLOPT_CONNECTTIMEOUT, 40L);

     h = curl_slist_append(h, "Accept: */*");
     if (body) { /* POST. */
	  h = curl_slist_append(h, "Content-Type: application/vnd.wap.mms-message");
	  curl_easy_setopt(cl, CURLOPT_POSTFIELDS, octstr_get_cstr(body));
	  curl_easy_setopt(cl, CURLOPT_POSTFIELDSIZE, octstr_len(body));
     }


     curl_easy_setopt(cl, CURLOPT_HTTPHEADER, h);
     curl_easy_setopt(cl, CURLOPT_ERRORBUFFER, errbuf);

     *hstatus = curl_easy_perform(cl); /* post away! */
     if (*hstatus != 0)
	  mms_error(0, "mmsbox-mm1", NULL, "failed to fetch/post content: %.256s",
		errbuf);

     curl_slist_free_all(h); /* free the header list */
     curl_easy_cleanup(cl);

     return s;
}

#include <unistd.h>

#define MAX_GPRS_WAIT 240
#define GPRS_POLL  20
static long start_gprs(Octstr *cmd, Octstr *pid_cmd)
{
     pid_t pid = fork();

     if (pid > 0)  {  /* parent. */
	  int ct = 0;
	  do {
	       FILE *f;
	       long xpid = -1, cpid;
	       int status;
	       gwthread_sleep(GPRS_POLL); /* wait a little. */
	       if ((f = popen(octstr_get_cstr(pid_cmd), "r")) != NULL) {
		    fscanf(f, "%ld", &xpid);
		    pclose(f);
		    if (xpid >= 0)
			 return xpid;
	       }
	       cpid = waitpid(pid, &status, WNOHANG); /* also wait for the child. */
	       mms_info(0, "mmsbox-mm1", NULL,  "waiting for connection: %d, pid=%ld cpid=%ld, ifexited=%d, exitstatus=%d",
		    ct, (long)pid, cpid, WIFEXITED(status), WEXITSTATUS(status));
	       if (cpid == pid &&
		   WIFEXITED(status))
		 return -1;
	  } while (GPRS_POLL*ct++ < MAX_GPRS_WAIT);
	  /* Timed out, but still need to wait for child pid, as
	  start-gprs script is still running and we don't need a
	  zombie */
	  pid_t rpid;
	  int st;

	  rpid = waitpid(pid, &st, 0);
	  mms_info(0, "mmsbox-mm1", NULL, "pid %d terminated", pid);
	  return -1;
    } else if (pid == 0) { /* child. */
	  List *l = octstr_split_words(cmd);
	  int i, n = gwlist_len(l);
	  char **args = gw_malloc((n+1) * sizeof *args);

	  for (i = 0; i < n; i++) {
	       Octstr *x = gwlist_get(l, i);
	       args[i] = octstr_get_cstr(x);
	       printf("arg %d: %s\n", i, args[i]);
	  }
	  args[i] = NULL;
	  printf("Not reached: %d!", execvp(args[0], args));
     } else
	  mms_error(0, "mmsbox-mm1", NULL, "failed to start process!");
     return -1;
}

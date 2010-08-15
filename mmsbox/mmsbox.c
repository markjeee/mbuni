/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBOX: MMS Content engine
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
#include <string.h>
#include <ctype.h>

#ifdef SunOS
#include <strings.h>
#include <fcntl.h>
#endif

#include "mms_queue.h"
#include "mms_uaprof.h"
#include "mmsbox.h"

/* XXX warning, do not octstr_destroy strings in HTTPCGIVar struct. They are destroyed by the http module! */

volatile sig_atomic_t rstop = 0;
static void quit_now(int notused)
{
  rstop = 1;
     
  mms_info(0, "mmsbox", NULL, "shutdown in progress...");
  /* Close all MMSC http ports, kill all MMSC threads, kill sendmms port... */     
  if (sendmms_port.port > 0)
    http_close_port(sendmms_port.port);

  mmsbox_stop_all_mmsc_conn();
}

/* manage the SIGHUP signal */
static void relog_now(int notused)
{
  mms_warning(0, "mmsbox", NULL, "SIGHUP received, catching and re-opening logs");
  log_reopen();
  alog_reopen();
}

/* Finds text part, returns copy. */
static MIMEEntity *find_textpart(MIMEEntity *m)
{
  Octstr *ctype = NULL, *params = NULL;
  MIMEEntity *res = NULL;
  List *headers;
  int i, n;
     
  if (!m) return NULL; 

  headers = mime_entity_headers(m);
  get_content_type(headers, &ctype, &params);
  http_destroy_headers(headers);
     
  if (ctype && octstr_str_compare(ctype, "text/plain") == 0) {
    res = mime_entity_duplicate(m);
    goto done;
  }
     
  if ((n = mime_entity_num_parts(m)) > 0) {
    for (i = 0; i < n; i++) {
      MIMEEntity *x = mime_entity_get_part(m, i);
      res = find_textpart(x);
      mime_entity_destroy(x);
	       
      if (res != NULL) 
	goto done2;
    }
  }
 done:
  if (res) { /* We got it! Convert charset if needed. */
    List *params_h = get_value_parameters(params);
    Octstr *charset = http_header_value(params_h, octstr_imm("charset"));
    Octstr *body = mime_entity_body(res);
    if (charset == NULL || 
	octstr_str_compare(charset, "unknown") == 0) {
      octstr_destroy(charset);
      charset = octstr_create(DEFAULT_CHARSET);
    }
	  
    if (octstr_str_case_compare(charset, DEFAULT_CHARSET) != 0) {
      charset_convert(body, DEFAULT_CHARSET, octstr_get_cstr(charset)); /* XXX error ignored? */	  
      mime_entity_set_body(res, body);
    }
    octstr_destroy(body);
    http_destroy_headers(params_h);
    octstr_destroy(charset);
  } 

 done2:
     
  octstr_destroy(ctype);     
  octstr_destroy(params);
  return res;
}


/* Gets the keyword, if any, from the text part of the message. 
 * converts charset as needed. 
 */
static Octstr *get_keyword(MIMEEntity *me)
{

  MIMEEntity *t = find_textpart(me);
  Octstr *txt = t ? mime_entity_body(t) : NULL;
  List *l = t ? octstr_split_words(txt) : NULL;
  Octstr *keyword = l ? gwlist_extract_first(l) : NULL;
     
  if (t)
    mime_entity_destroy(t);
  gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
  octstr_destroy(txt);

  return keyword;
}

static int _x_octstr_comp(Octstr *x, Octstr *y)
{
  return (octstr_case_compare(x,y) == 0);
}

static MmsService *get_service(Octstr *keyword, Octstr *mmc_id, Octstr *receiver)
{
  int i, n;
  MmsService *catch_all = NULL;
  Octstr *phonenum = receiver ? extract_phonenum(receiver, NULL) : NULL;
     
  for (i = 0, n = gwlist_len(mms_services); i < n; i++) {
    MmsService *ms = gwlist_get(mms_services,i);
	  
    /* Check that mmc_id is allowed: 
     * denied list is not null and we are on it, or allowed list is not null and we 
     * are *not* on it. 
     */
    if (ms->denied_mmscs &&
	gwlist_search(ms->denied_mmscs, mmc_id, (gwlist_item_matches_t *)_x_octstr_comp) != NULL)
      continue;
	  
    if (ms->allowed_mmscs &&
	gwlist_search(ms->allowed_mmscs, mmc_id, (gwlist_item_matches_t *)_x_octstr_comp) == NULL)
      continue;
	  
    if (ms->denied_receiver_prefix && phonenum  && 
	does_prefix_match(ms->denied_receiver_prefix, phonenum) != 0)
      continue;
	  
    if (ms->allowed_receiver_prefix && phonenum  && 
	does_prefix_match(ms->allowed_receiver_prefix, phonenum) == 0)
      continue;
	  
    if (keyword &&
	gwlist_search(ms->keywords, keyword, 
		      (gwlist_item_matches_t *)_x_octstr_comp) != NULL) {
      octstr_destroy(phonenum);
      return ms;
    }
	  
    if (ms->isdefault && catch_all == NULL) /* We also find the catch-all for this sender. */
      catch_all = ms;
  }
     
  octstr_destroy(phonenum);
  return catch_all;
}

static void add_all_matching_parts(MIMEEntity *plist, MmsServiceUrlParam *pm, 
				   Octstr *keyword,
				   MIMEEntity *me, 
				   MmsMsg *msg, int lev, int count)
{
  int i, n;
  List   *headers = NULL;
  Octstr *data = NULL, *ctype = NULL, *xctype = NULL, *params = NULL;
  Octstr *s;
     
  headers = mime_entity_headers(me);
  if (pm->type == WHOLE_BINARY && lev == 0) { 
    data = mms_tobinary(msg);
    ctype = octstr_create("application/vnd.wap.mms-message");
  } else if (pm->type == NO_PART && lev == 0) {
    data = octstr_create(""); /* We'll add value below. */
    goto done;
  } else if (pm->type == KEYWORD_PART && lev == 0) {
    data = keyword ? octstr_duplicate(keyword) : octstr_create(""); 
    ctype = octstr_create("text/plain");
    goto done;
  }
     
  if ((n = mime_entity_num_parts(me)) > 0) { /* Recurse over multi-parts. */
    for (i = 0; i < n; i++) {
      MIMEEntity *x = mime_entity_get_part(me,i);
      add_all_matching_parts(plist, pm, NULL, x, msg, lev+1,i);
      mime_entity_destroy(x);
    }
    goto done;
  }

  get_content_type(headers, &xctype, &params);

#define BEGINSWITH(s, prefix) (octstr_case_search(s, octstr_imm(prefix),0) == 0)     
#define TYPE_MATCH(typ, prefix) ((pm->type) == (typ) &&		\
				 BEGINSWITH(xctype, prefix))
  if (xctype)
    if (TYPE_MATCH(IMAGE_PART,"image/") ||
	TYPE_MATCH(AUDIO_PART,"audio/") ||
	TYPE_MATCH(VIDEO_PART,"video/") ||
	TYPE_MATCH(TEXT_PART,"text/") ||
	TYPE_MATCH(SMIL_PART,"application/smil") ||
	pm->type == ANY_PART ||     /* Skip top-level for 'any' parts. */
	(pm->type == OTHER_PART && 
	 (!BEGINSWITH(xctype, "text/") && 
	  !BEGINSWITH(xctype, "video/") &&  
	  !BEGINSWITH(xctype, "image/") &&  
	  !BEGINSWITH(xctype, "application/smil")))) {
	       
      ctype = http_header_value(headers, octstr_imm("Content-Type"));
      data = mime_entity_body(me);
    }
     
 done:    
  if (data) {
    MIMEEntity *p = mime_entity_create();
    Octstr *cd = octstr_format("form-data; name=\"%S\"", pm->name);
    List *xh;
	  
    if (ctype && pm->type != KEYWORD_PART) {
      /* If Content-Location header or name (content-type) parameter given, pass it as filename. */
      Octstr *c = NULL, *q = NULL;
      Octstr *cloc =  http_header_value(headers,
					octstr_imm("Content-Location"));    
      split_header_value(ctype, &c, &q);
	       
      if (q || cloc) {
	List *ph = q ? get_value_parameters(q) : http_create_empty_headers();
	Octstr *v = cloc ? cloc : http_header_value(ph, octstr_imm("name"));
		    
	if (!v) /* make up a fake name if none is given */
	  v = octstr_format("part-%d-%d", lev, count);
		    
	octstr_format_append(cd, "; filename=\"%S\"", v);
	http_header_remove_all(ph, "name");
		    
	if (v != cloc)
	  octstr_destroy(v);
	octstr_destroy(ctype);

	v  = make_value_parameters(ph);
	if (v && octstr_len(v) > 0)
	  ctype = octstr_format("%S; %S", c, v);
	else 
	  ctype = octstr_duplicate(c);			      

	octstr_destroy(v);
		    
	http_destroy_headers(ph);

	octstr_destroy(q);
      }
	       

      octstr_destroy(c);
      octstr_destroy(cloc);
    }

    xh = http_create_empty_headers();
    http_header_add(xh, "Content-Disposition", octstr_get_cstr(cd));	  
    if (ctype) /* This header must come after the above it seems. */
      http_header_add(xh, "Content-Type", octstr_get_cstr(ctype));
	  
    mime_replace_headers(p, xh);
    http_destroy_headers(xh);
	  
    s = octstr_duplicate(data); /* data for the parameter */
    if (pm->value) /* add value part as needed. */
      octstr_append(s, pm->value);
    mime_entity_set_body(p, s);
    octstr_destroy(s);	       
#if 0
    base64_mimeparts(p);
#endif
    mime_entity_add_part(plist, p); /* add it to list so far. */
    mime_entity_destroy(p);

    octstr_destroy(cd);
  }


  octstr_destroy(xctype);     
  octstr_destroy(params);
  octstr_destroy(ctype);
  octstr_destroy(data);
     
  http_destroy_headers(headers);
}

enum _xurltype {FILE_TYPE, URL_TYPE};
static Octstr *url_path_prefix(Octstr *url, int type);

/* this function constructs and sends the message -- a bit too many params XXX! */
static int make_and_queue_msg(Octstr *data, Octstr *ctype, List *reply_headers, 
			      Octstr *msg_url,
			      Octstr *base_url, int type, MmsEnvelope *e,
			      Octstr *svc_name, Octstr *faked_sender, Octstr *service_code,
			      int accept_x_headers, 
			      List *passthro_headers,
			      Octstr *qdir,
			      Octstr **err);
static int fetch_serviceurl(MmsEnvelope *e,      
			    MmsService *ms, MmsMsg *m, 
			    MIMEEntity *msg, 
			    Octstr *keyword,
			    Octstr **err)
{
  List *rh, *rph = NULL;
  Octstr *body = NULL, *rb = NULL, *transid;
  Octstr *ctype = NULL, *params = NULL;
  Octstr *s;
  int i, n, method, typ = FILE_TYPE;
  FILE *fp = NULL;

  int res = -1;

  transid = mms_maketransid(e->xqfname, octstr_imm(MM_NAME));
  switch (ms->type) {
  case TRANS_TYPE_GET_URL:
  case TRANS_TYPE_POST_URL:
    rh = http_create_empty_headers();
	  
    http_header_add(rh, "User-Agent", MM_NAME "/" VERSION);	       
    /* Put in some useful headers. */
    if (e->msgId)
      http_header_add(rh, "X-Mbuni-Message-ID", octstr_get_cstr(e->msgId));
    if (e->fromproxy) 
      http_header_add(rh, "X-Mbuni-MMSC-ID", octstr_get_cstr(e->fromproxy));
    if (e->token)
      http_header_add(rh, "X-Mbuni-LinkedID", octstr_get_cstr(e->token));
    if (e->from)	{       
      Octstr *xfrom = octstr_duplicate(e->from);
	       
      _mms_fixup_address(&xfrom, unified_prefix ? octstr_get_cstr(unified_prefix) : NULL, 
			 strip_prefixes, 0);	       
      http_header_add(rh, "X-Mbuni-From", octstr_get_cstr(xfrom));
      octstr_destroy(xfrom);
    }
    if (e->subject)	       
      http_header_add(rh, "X-Mbuni-Subject", octstr_get_cstr(e->subject));
	  
    /* Put in a transaction ID. */
    http_header_add(rh, "X-Mbuni-TransactionID", octstr_get_cstr(transid));

    for (i = 0, n = gwlist_len(e->to); i < n; i++) {
      MmsEnvelopeTo *r = gwlist_get(e->to, i);
      if (r && r->rcpt) {
	Octstr *xto = octstr_duplicate(r->rcpt);
	_mms_fixup_address(&xto, unified_prefix ? octstr_get_cstr(unified_prefix) : NULL, 
			   strip_prefixes, 0);	       		    
	http_header_add(rh, "X-Mbuni-To", octstr_get_cstr(xto));
	octstr_destroy(xto);
      }
    }

    if ((s = http_header_value(e->hdrs, octstr_imm("X-Mbuni-UAProf"))) != NULL) { /* add UAProf info, if any. */
      Octstr *sx = http_header_value(e->hdrs,octstr_imm("X-Mbuni-Timestamp"));
      http_header_add(rh, "X-Mbuni-UAProf", octstr_get_cstr(s));
      if (sx) 
	http_header_add(rh, "X-Mbuni-Timestamp", octstr_get_cstr(sx));
      octstr_destroy(sx);
      octstr_destroy(s);
    }

    if ((s = mms_get_header_value(m, octstr_imm("Date"))) != NULL) {	       
      http_header_add(rh, "X-Mbuni-Message-Date", octstr_get_cstr(s));
      octstr_destroy(s);
    }
	  
    s = date_format_http(e->created);
    http_header_add(rh, "X-Mbuni-Received-Date", octstr_get_cstr(s));
    octstr_destroy(s);
	  
    if (ms->type == TRANS_TYPE_POST_URL)  { /* Put in the parameters. */
      MIMEEntity *x = mime_entity_create();
	       
      http_header_add(rh, "Content-Type", "multipart/form-data");
      mime_replace_headers(x, rh);
      http_destroy_headers(rh);
	       
      for (i = 0, n = gwlist_len(ms->params); i < n; i++) {
	MmsServiceUrlParam *p = gwlist_get(ms->params, i);		    
	add_all_matching_parts(x, p, keyword, msg, m, 0, i);
      }

      body = mime_entity_body(x);
      rh = mime_entity_headers(x);

      mime_entity_destroy(x);
	       
      method = HTTP_METHOD_POST;
    } else 
      method = HTTP_METHOD_GET;
	  
    typ = URL_TYPE;
    if (mmsbox_url_fetch_content(method, ms->url, rh, body, &rph, &rb) == HTTP_OK) {
      get_content_type(rph, &ctype, &params);
      /* add transaction id back.*/
      http_header_remove_all(rph, "X-Mbuni-TransactionID");
      http_header_add(rph, "X-Mbuni-TransactionID", octstr_get_cstr(transid)); 
    } else 
      *err = octstr_format("MMSBox: Failed to fetch content for Service %S, url %S!",
			   ms->name, ms->url);
    http_destroy_headers(rh);
    octstr_destroy(body);
    break;
  case TRANS_TYPE_FILE:
    if ((fp = fopen(octstr_get_cstr(ms->url), "r")) != NULL) {	  
      rb = octstr_read_pipe(fp);	  
      fclose(fp);	  	  
      ctype = filename2content_type(octstr_get_cstr(ms->url));
    } 

    if (!rb)
      *err = octstr_format("MMSBox: Failed to open file %S for service %S!", 
			   ms->url, ms->name);
    typ = FILE_TYPE;   
    break;
  case TRANS_TYPE_EXEC:
    if ((fp = popen(octstr_get_cstr(ms->url), "r+")) != NULL) {
      Octstr *s = mime_entity_to_octstr(msg);
      if (s) { /* Send the MMS to the exec program */
	octstr_print(fp, s);
	fflush(fp);		    
	octstr_destroy(s);
      }
      rb = octstr_read_pipe(fp);
      ctype = octstr_create("application/smil");
      pclose(fp);
    }
    if (!rb)
      *err = octstr_format("MMSBox: Failed to fetch content for Service %S, exec path = %S!",
			   ms->name, ms->url);	  
    typ = FILE_TYPE;
    break;
  case TRANS_TYPE_TEXT:
    rb = octstr_duplicate(ms->url);
    ctype = octstr_create("text/plain");
    typ = URL_TYPE;
    break;

  default:
    *err = octstr_format(0, "MMSBOX: Unknown Service type for service %S!", ms->name);
    break;
  }
     
  /* If we have the content, make the message and write it to out-going. 
   * if we have no content, or we have been told to suppress the reply, then we suppress
   */
  if (!ctype ||  !rb || 
      (octstr_len(rb) == 0 && ms->omitempty) ||
      ms->noreply) {
    mms_info(0,  "mmsbox", NULL, "MMSBox.service_request: Suppressed reply for service %s, "
	     "suppress-reply=%s, omit-empty=%s, reply-len=%ld, content-type=%s",
	     octstr_get_cstr(ms->name), ms->noreply ? "true" : "false",
	     ms->omitempty ? "true" : "false",
	     rb ? octstr_len(rb) : 0, 
	     ctype ? octstr_get_cstr(ctype) : "");
    goto done;
  } else {
    Octstr *base_url = url_path_prefix(ms->url, typ);

    if (ms->special_header) {
      if (rph == NULL) 
	rph = http_create_empty_headers();
      http_header_remove_all(rph, "X-Mbuni-Content-Header");
      http_header_add(rph, "X-Mbuni-Content-Header", octstr_get_cstr(ms->special_header));
    }
    res = make_and_queue_msg(rb, ctype, rph, 
			     ms->url,
			     base_url, 
			     typ, e, ms->name, ms->faked_sender, ms->service_code,
			     ms->accept_x_headers, ms->passthro_headers, 
			     outgoing_qdir,
			     err);
	  
    octstr_destroy(base_url);
  }

 done:     
  octstr_destroy(ctype);
  octstr_destroy(rb);
  http_destroy_headers(rph);
  octstr_destroy(params);
  octstr_destroy(transid);
     
  return res;
}


static int mmsbox_send_report(Octstr *from, char *report_type,
			      Octstr *dlr_url, Octstr *status, 
			      Octstr *msgid, Octstr *orig_msgid, 
			      Octstr *mmc_id, Octstr *mmc_gid,
			      Octstr *orig_transid, Octstr *uaprof, 
			      Octstr* uaprof_tstamp,
			      Octstr *err, 
			      Octstr *statustxt, Octstr *details)
{     
  List *rh = NULL, *rph = NULL;
  Octstr *rb = NULL, *xfrom = from ? octstr_duplicate(from) : NULL;
  int ret = HTTP_NOT_FOUND;
     
     
  if (xfrom)
    _mms_fixup_address(&xfrom, unified_prefix ? octstr_get_cstr(unified_prefix) : NULL, 
		       strip_prefixes, 0);
     
  rh = http_create_empty_headers();
     
  http_header_add(rh, "X-Mbuni-Report-Type", report_type);
  http_header_add(rh, "X-Mbuni-MM-Status", octstr_get_cstr(status));
  if (mmc_gid) 
    http_header_add(rh, "X-Mbuni-MMSC-GID", octstr_get_cstr(mmc_gid));
  http_header_add(rh, "X-Mbuni-MMSC-ID", octstr_get_cstr(mmc_id));
  if (xfrom) 
    http_header_add(rh, "X-Mbuni-From", octstr_get_cstr(xfrom));

  if (orig_transid)
    http_header_add(rh, "X-Mbuni-TransactionID", 
		    octstr_get_cstr(orig_transid));
  if (msgid)
    http_header_add(rh, "X-Mbuni-Message-ID", octstr_get_cstr(msgid));

  if (orig_msgid)
    http_header_add(rh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(orig_msgid));


  if (err)
    http_header_add(rh, "X-Mbuni-MM-Error", octstr_get_cstr(err));

  if (statustxt)
    http_header_add(rh, "X-Mbuni-MM-StatusText", octstr_get_cstr(statustxt));

  if (details)
    http_header_add(rh, "X-Mbuni-MM-StatusDetails", octstr_get_cstr(details));

  if (uaprof) {
    http_header_add(rh, "X-Mbuni-UAProf", octstr_get_cstr(uaprof));
    http_header_add(rh, "X-Mbuni-Timestamp", octstr_get_cstr(uaprof_tstamp));
  }
  ret =  mms_url_fetch_content(HTTP_METHOD_GET, dlr_url, rh, NULL, &rph, &rb);

  octstr_destroy(rb);	  
  octstr_destroy(xfrom);

  http_destroy_headers(rph);
  http_destroy_headers(rh);


  return http_status_class(ret) == HTTP_STATUS_SUCCESSFUL ? 0 : -1;
}

static int mmsbox_service_dispatch(MmsEnvelope *e)
{
  MmsMsg *msg = NULL;
  MIMEEntity *me = NULL;
  int i, n, res = 0;
  time_t tnow = time(NULL);
  Octstr *err = NULL, *keyword = NULL;
  MmsService *ms;
  MmsEnvelopeTo *xto;
  Octstr *prio = NULL, *mclass = NULL, *mstatus = NULL;
     
  gw_assert(e->msgtype == MMS_MSGTYPE_SEND_REQ || 
	    e->msgtype == MMS_MSGTYPE_RETRIEVE_CONF || 
	    e->msgtype == MMS_MSGTYPE_DELIVERY_IND || 
	    e->msgtype == MMS_MSGTYPE_READ_ORIG_IND);
     
  if ((msg = qfs->mms_queue_getdata(e)) == NULL) {
    err = octstr_format("Failed to read message for queue entry %s!",
			e->xqfname);
    res = -1;
    goto done;
  } else if (e->expiryt != 0 &&  /* Handle message expiry. */
	     e->expiryt < tnow) {
    err = octstr_format("Queue entry [msgid=%S,mmc_id=%S] "
			"expired while sending to service!", e->msgId, e->fromproxy);
    res = -1;
	  
    goto done;
  } else if (e->attempts >= svc_maxsendattempts) {
    err = octstr_format("Failed to deliver [msgid=%S,mmc_id=%S] "
			"after %ld attempts. (max attempts allowed is %ld)!", 
			e->msgId, e->fromproxy, e->attempts, 
			svc_maxsendattempts);
    res = -1;
    goto done;
  } 

  mclass = mms_get_header_value(msg, octstr_imm("X-Mms-Message-Class"));
  prio = mms_get_header_value(msg, octstr_imm("X-Mms-Priority"));
  mstatus  = mms_get_header_value(msg, octstr_imm("X-Mms-Status"));
     
  if (e->msgtype == MMS_MSGTYPE_DELIVERY_IND || 
      e->msgtype == MMS_MSGTYPE_READ_ORIG_IND) {
    char  *report_type  = (e->msgtype == MMS_MSGTYPE_DELIVERY_IND) ? "delivery-report" : "read-report";
    Octstr *msgid = mms_get_header_value(msg, octstr_imm("Message-ID"));
    Octstr *orig_msgid = e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-Orig-Message-ID")) : NULL;
    Octstr *status =   mms_get_header_value(msg,
					    e->msgtype == MMS_MSGTYPE_DELIVERY_IND ? 
					    octstr_imm("X-Mms-Status") :
					    octstr_imm("X-Mms-Read-Status"));
    Octstr *orig_transid = e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-TransactionID")) : NULL;
    Octstr *uaprof =  e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-UAProf")) : NULL;
	  
    Octstr *tstamp =  e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-Timestamp")) : NULL;
    Octstr *gid  =  e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-Mmsc-GroupID")) : NULL;

    Octstr *err  =  e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-Error")) : NULL;
    Octstr *statustxt  =  e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-StatusText")) : NULL;
    Octstr *details  =  e->hdrs ? 
      http_header_value(e->hdrs, octstr_imm("X-Mbuni-StatusDetails")) : NULL;

    if (e->url1)
      res = mmsbox_send_report(e->from, report_type, 
			       e->url1, status, msgid, orig_msgid, 
			       e->fromproxy, gid, orig_transid, 
			       uaprof,tstamp, err, statustxt, details);
    else {
      mms_info(0, "MM7", e->fromproxy, "MMSBox: Skipped %s URL call for [%s]. Empty URL, from [%s]", 
	       report_type, octstr_get_cstr(msgid), octstr_get_cstr(e->from));
      res = 0;
    }
	  
    octstr_destroy(msgid);
    octstr_destroy(orig_msgid);
    octstr_destroy(status);
    octstr_destroy(orig_transid);
    octstr_destroy(uaprof);
    octstr_destroy(tstamp);
    octstr_destroy(gid);
    octstr_destroy(err);
    octstr_destroy(statustxt);
    octstr_destroy(details);
    goto done; /* No more processing. */
  }
     
  if (gwlist_len(e->to) == 0) { /* nothing to do. odd XXX */	  
    res = 0;
    goto done;
  }

  me = mms_tomime(msg, 0);
  keyword = get_keyword(me);

  xto = gwlist_get(e->to, 0);
  ms = get_service(keyword, e->fromproxy, xto ? xto->rcpt : NULL);
     
  if (!ms) {
    err = octstr_format("No Service to handle %S (keyword %S)!", 
			e->msgId, keyword ? octstr_imm("") : keyword);
    res = -1;
    goto done;
  }
     
  e->_x = ms; /* Remember it for later. */
     
  res = fetch_serviceurl(e, ms, msg, me, keyword, &err);
 done:
  
  if (err && res != 0) 
    mms_error(0, "mmsbox", NULL, "Error: %s", octstr_get_cstr(err));
	  
  if (res == -1 || res == 0)   /* Fatal error, or success delete queue entry. */
    for (i = 0, n = gwlist_len(e->to); i < n; i++) {

      MmsEnvelopeTo *r = gwlist_get(e->to,i);    
      if (r) 
	r->process = 0;
      /* Do CDR */
      cdrfs->logcdr(e->created, 
		    octstr_get_cstr(e->from),
		    octstr_get_cstr(r->rcpt),
		    octstr_get_cstr(e->msgId),
		    e->fromproxy ? octstr_get_cstr(e->fromproxy) : NULL,
		    e->src_interface, 
		    "mms-service",
		    e->msize, 
		    (char *)mms_message_type_to_cstr(e->msgtype),
		    prio ? octstr_get_cstr(prio) : NULL,
		    mclass ? octstr_get_cstr(mclass) : NULL,
		    mstatus ? octstr_get_cstr(mstatus) : 
		    (res == 0 ? "forwarded" : "dropped"),
		    e->dlr,
		    0);
    }
  else { /* Not succeeded so we need to wait a bit and try later. */
    e->lasttry = time(NULL);
    e->attempts++;  /* Update count of number of delivery attempts. */   
    e->sendt = e->lasttry + mmsbox_send_back_off * e->attempts;
  }
     
  if (qfs->mms_queue_update(e) != 1)
    qfs->mms_queue_free_env(e);	  

  octstr_destroy(err);
  octstr_destroy(keyword);
  mms_destroy(msg);

  octstr_destroy(prio);
  octstr_destroy(mclass);
  octstr_destroy(mstatus);

  if (me)
    mime_entity_destroy(me);
  return 1;
}

static void sendmms_func(void *unused);
int main(int argc, char *argv[])
{
  Octstr *fname;
  int cfidx;
     
  long qthread = 0, sthread = 0;

  mms_lib_init();

  srandom(time(NULL));

  cfidx = get_and_set_debugs(argc, argv, NULL);

  if (argv[cfidx] == NULL)
    fname = octstr_create("mbuni.conf");
  else 
    fname = octstr_create(argv[cfidx]);
     
  if (mms_load_mmsbox_settings(fname,(gwthread_func_t *)mmsc_receive_func) < 0)
    panic(0, "Configuration file loading failed, file: %s", octstr_get_cstr(fname));

  octstr_destroy(fname);

  mms_info(0,  "mmsbox", NULL,"----------------------------------------");
  mms_info(0,  "mmsbox", NULL," " MM_NAME " MMSBox  version %s starting", MMSC_VERSION);
     
     
#ifdef SA_RESTART
  {
    struct sigaction nact;
       
    memset(&nact, 0, sizeof(nact));
    nact.sa_handler = relog_now;
    nact.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &nact, (struct sigaction *)0);
  }
#else
  signal(SIGHUP, relog_now);
#endif
  signal(SIGTERM, quit_now);
  signal(SIGINT,  quit_now);   
  signal(SIGPIPE,SIG_IGN); /* Ignore pipe errors. They kill us sometimes for nothing*/
     
  /* Start sendmms port */     
  if (sendmms_port.port > 0)
    sthread = gwthread_create((gwthread_func_t *)sendmms_func, NULL);


  /* Start out-going queue thread. */
  qthread = gwthread_create((gwthread_func_t *)mmsbox_outgoing_queue_runner, &rstop);     

  qfs->mms_queue_run(octstr_get_cstr(incoming_qdir), 
		     mmsbox_service_dispatch, 
		     queue_interval, maxthreads, &rstop);
     
  mms_info(0,  "mmsbox", NULL,"Shutdown started..");

  sleep(2);     
  /* Wait for the sender thread, then quit. */
  if (qthread >= 0)
    gwthread_join(qthread); /* Wait for it to die... */
     
  if (sendmms_port.port > 0 && sthread >= 0)
    gwthread_join(sthread);
     
  mmsbox_settings_cleanup();
  mms_info(0, "mmsbox", NULL, "Shutdown complete..");

  mms_lib_shutdown();
  return 0;
}



/* Return the prefix part of a url or file. */
static Octstr *url_path_prefix(Octstr *url, int type)
{
  int i, j,  len = octstr_len(url);
  char *p = octstr_get_cstr(url);

  /* Set lower/upper limit of search. */
  if (type == URL_TYPE)  { /* then skip first slashes. */
    char *x;
    i = octstr_search(url, octstr_imm("://"),0);
    if (i > 0)
      i += 3;
    else 
      i = 0;
    x = rindex(p, '#'); /* look for fragment if any. */
	  
    if (x)
      j = x - p  - 1;
    else 
      j = len - 1;
  } else {
    i = 0;
    j = len - 1;
  }

  /* Now search backwards for the last '/'. 
   * if you don't find one, set to end of string. 
   */
     
  for (;j > i; j--)
    if (p[j] == '/')
      break;
  if (j <= i)
    j = len;

  return octstr_copy(url, 0, j);
}

/* Get's just the host:port part, leaving out UrI */
static Octstr *get_toplevel_url(Octstr *url)
{
  int i, len = octstr_len(url);
  char *p = octstr_get_cstr(url);
     
  i = octstr_search(url, octstr_imm("://"),0);
     
  if (i > 0)
    i += 3;
  else 
    i = 0;

  for ( ; i < len; i++)
    if (p[i] == '/')
      break;
     
  return octstr_copy(url, 0, i);     
}

/* little dirty method to see if file begins with url scheme. */
static int has_url_scheme(char *url, int *supported_scheme)
{
  char *p = strstr(url, "://");

  *supported_scheme = 1;
     
  if (strstr(url, "data:") == url || /* data: url scheme */
      strstr(url, "http://") == url ||
#ifdef HAVE_LIBSSL
      strstr(url, "https://") == url ||
#endif
      strstr(url, "file://") == url)
    return 1;
     
  if (p) {

    for (p--; p >= url; p--)
      if (!isalpha(*p))
	break;
    if (p < url) {
      *supported_scheme = 0; /* we don't support this one. */
      return 1;
    } else 
      return 0;	  
  } else 
    return 0;
}

static int add_msg_part(MIMEEntity *res, xmlNodePtr node, Octstr *base_url,
			Octstr *top_url,  
			int type, Octstr *svc_name,
			Octstr *mmc_id,
			Dict *url_map, Octstr *xmhdr, Octstr *xmhdr_val)
{
  Octstr *curl = NULL, *ctype = NULL, *body = NULL, *xcid = NULL;
  char *src = NULL;
  int isurl, slash_prefix;
  Octstr *cid = NULL;
  int supported_url_scheme = 0;
     
  /* For each node in the smil doc, if it has an src attribute, then: 
   * - if our type of base_url is FILE *and the src attribute does not look 
   *   like a url, then file the file referenced, load it into the message and go
   * - if our type is URL and the url scheme is http/https (or has no scheme)
   *   then fetch it and put into message. 
   */
     
  if (!node || node->type != XML_ELEMENT_NODE ||
      (src = (char *)xmlGetProp(node, (unsigned char *)"src")) == NULL)
    return 0; /* Nothing to do. */
     
  if (src[0] == '\\') { /* User can escape url to prevent substitution. */
    xmlSetProp(node, (xmlChar *)"src", (xmlChar *)(src + 1));
    goto done;
  }

  isurl = has_url_scheme(src, &supported_url_scheme);
  slash_prefix = (src[0] == '/');

  if (isurl && !supported_url_scheme)
    goto done;
  else if (isurl)
    curl = octstr_create(src);
  else if (slash_prefix) {
    if (type == URL_TYPE)
      curl = octstr_format("%S%s",
			   top_url, src);
    else 
      curl = octstr_create(src);	       	       
  } else 
    curl = octstr_format("%S/%s",base_url, src);

  if ((cid = dict_get(url_map, curl)) != NULL) { /* We've seen it before. */
    xmlSetProp(node, (xmlChar *)"src", (xmlChar *)octstr_get_cstr(cid));
    /* Don't delete cid! */
    goto done;
  }
     
  isurl |= (type == URL_TYPE); /* From now on, this flag tells us whether we are fetching a url.*/

  if (isurl) {
    List *rh = http_create_empty_headers(), *rph = NULL;
	  
    http_header_add(rh, "User-Agent", MM_NAME "/" VERSION);	       
    if (mmsbox_url_fetch_content(HTTP_METHOD_GET, curl, rh, NULL, &rph, &body) == HTTP_OK) {
      ctype = http_header_value(rph, octstr_imm("Content-Type"));
      xcid = http_header_value(rph, octstr_imm("Content-ID"));
    } else 
      mms_error(0,  "mmsbox", NULL, "Failed to load url %s within SMIL content from service %s!",
		octstr_get_cstr(curl), 
		svc_name ? octstr_get_cstr(svc_name) : "unknown");
    if (rph)
      http_destroy_headers(rph);
    http_destroy_headers(rh);
  } else {
    body = octstr_read_file(octstr_get_cstr(curl));
    ctype = filename2content_type(src);
  }
     
  if (ctype && body) { /* If we got it, put it in. */
    static int cntr;  /* For generating cids */
    char _fext[5] = {0}, *fext = make_file_ext(curl, ctype, _fext);
    Octstr *attr = xcid ? octstr_format("cid:%S", xcid) : octstr_format("cid:%06d.%s", ++cntr,fext);
    char  *p = octstr_get_cstr(attr) + 4;
    Octstr *cid_header_val = octstr_format("<%s>", p);
    MIMEEntity *x = mime_entity_create();
    List *headers = http_create_empty_headers();
	  
    http_header_add(headers, "Content-Type", octstr_get_cstr(ctype));
    http_header_add(headers, "Content-ID", octstr_get_cstr(cid_header_val));
    http_header_add(headers, "Content-Location", p);

    if (xmhdr)
      http_header_add(headers, octstr_get_cstr(xmhdr), octstr_get_cstr(xmhdr_val));		    
	  
    mime_replace_headers(x, headers);
    mime_entity_set_body(x, body);
	  
	  
    if (mt_filter && mmc_id)
      mt_filter->filter(&x, curl, mmc_id);

    mime_entity_add_part(res, x);
    mime_entity_destroy(x);

    dict_put_once(url_map, curl, octstr_duplicate(attr)); /* Store the cid. */

    xmlSetProp(node, (xmlChar *)"src", (xmlChar *)octstr_get_cstr(attr));

    octstr_destroy(attr);
    octstr_destroy(cid_header_val);
    http_destroy_headers(headers);
  }

 done:

  octstr_destroy(curl);
  octstr_destroy(ctype);
  octstr_destroy(body);
  octstr_destroy(xcid);
  xmlFree(src);
  return 0;
}

/* Traverse the tree doing the above. */
static void add_msg_parts(MIMEEntity *res, xmlNodePtr node, Octstr *base_url, 
			  Octstr *top_url, 
			  int type, Octstr *svc_name, 
			  Octstr *mmc_id,
			  Dict *url_map, Octstr *xmhdr, Octstr *xmhdr_val)
{
  xmlNodePtr n;
  /* Do all the children recursively, then come back and do parent. */
  for (n = node; n; n = n->next)
    if (n->type != XML_COMMENT_NODE) {
      add_msg_part(res, n, base_url,  top_url, type, svc_name, mmc_id, url_map, xmhdr, xmhdr_val);
      add_msg_parts(res, n->xmlChildrenNode, base_url, top_url, type, 
		    svc_name, mmc_id, url_map, xmhdr, xmhdr_val);	       
    }          
}

/* Given content, make a message. We'll also use this for send-mms-user! */
static int make_and_queue_msg(Octstr *data, Octstr *ctype, List *reply_headers, 
			      Octstr *msg_url,
			      Octstr *base_url, int type, MmsEnvelope *e, 
			      Octstr *svc_name, Octstr *faked_sender, Octstr *service_code,
			      int accept_x_headers, List *passthro_headers,
			      Octstr *qdir,
			      Octstr **err)
{
  Octstr *from  = NULL, *xfrom = NULL, *subject = NULL, *turl = get_toplevel_url(base_url);
  Octstr *dlr_url = NULL, *rr_url = NULL, *mmc = NULL, *xservice_code = NULL, *hsvc_code = NULL;
  Octstr *allow_adaptations = NULL, *mclass = NULL, *prio = NULL, *distro = NULL, *chargedparty = NULL;
  MmsMsg *m = NULL;
  MIMEEntity *me = mime_entity_create();
  List *xheaders = NULL;
  List *hdrs = http_create_empty_headers();	  
  Octstr *otransid = NULL;
  Octstr *xmhdr = NULL, *xmhdr_val = NULL;

  time_t expiryt = time(NULL) + DEFAULT_EXPIRE;
  Octstr *x;
  List *xto = gwlist_create();
  int i, n, res = -1;
     
  gw_assert(svc_name);

  /* Get headers needed, if we are allowed to do so. */
  if (accept_x_headers && reply_headers) {
    Octstr *x = NULL;
    List *l = NULL;
    subject = http_header_value(reply_headers, octstr_imm("X-Mbuni-Subject"));

    if ((x = http_header_value(reply_headers, octstr_imm("X-Mbuni-Expiry"))) != NULL)
      expiryt = date_parse_http(x);
	  
    if ((l = http_header_find_all(reply_headers, "X-Mbuni-To")) != NULL) {
      int i, n;	       
      for (i = 0, n = gwlist_len(l); i<n; i++) {
	Octstr *h = NULL, *v  = NULL;
	List *hv = NULL;
	int j;
	http_header_get(l, i, &h, &v);
		    
	hv = http_header_split_value(v);
		    
	for (j = 0; j < gwlist_len(hv); j++) {		    
	  Octstr *v = gwlist_get(hv, j);
	  /* Fix the address. */
	  _mms_fixup_address(&v, unified_prefix ? octstr_get_cstr(unified_prefix) : NULL, 
			     strip_prefixes, 1);
	  gwlist_append(xto, v);
	}
	octstr_destroy(v);
	octstr_destroy(h);
	gwlist_destroy(hv, NULL); /* Don't kill strings since we added them to xto above! */
      }
      http_destroy_headers(l);
    }

    xfrom = http_header_value(reply_headers, octstr_imm("X-Mbuni-From"));
    dlr_url = http_header_value(reply_headers, octstr_imm("X-Mbuni-DLR-Url"));
    rr_url = http_header_value(reply_headers, octstr_imm("X-Mbuni-RR-Url"));
    allow_adaptations = http_header_value(reply_headers, octstr_imm("X-Mbuni-Allow-Adaptations"));

    mmc = http_header_value(reply_headers, octstr_imm("X-Mbuni-MMSC"));
    mclass = http_header_value(reply_headers, octstr_imm("X-Mbuni-MessageClass"));
    prio = http_header_value(reply_headers, octstr_imm("X-Mbuni-Priority"));
    hsvc_code = http_header_value(reply_headers, octstr_imm("X-Mbuni-ServiceCode"));
    chargedparty = http_header_value(reply_headers, octstr_imm("X-Mbuni-Charged-Party"));
  }     

  if (reply_headers) { 
    /* always capture transid and distro */
    otransid = http_header_value(reply_headers, octstr_imm("X-Mbuni-TransactionID"));
    distro = http_header_value(reply_headers, octstr_imm("X-Mbuni-DistributionIndicator"));
  }
  if (gwlist_len(xto) == 0 && e && e->from) 
    gwlist_append(xto, octstr_duplicate(e->from));
     
  if (!subject && e && e->subject)
    subject = octstr_duplicate(e->subject);
     
  /* Look for special header name/value to be inserted on all objects. */
  if (reply_headers && 
      (xmhdr = http_header_value(reply_headers, octstr_imm("X-Mbuni-Content-Header"))) != NULL) {
    int x = octstr_search_char(xmhdr,':', 0);

    if (x > 0) {
      xmhdr_val = octstr_copy(xmhdr, x+1, octstr_len(xmhdr));
      octstr_delete(xmhdr, x, octstr_len(xmhdr));
    } else 
      xmhdr_val = octstr_create("");

    octstr_strip_blanks(xmhdr_val);
    octstr_strip_blanks(xmhdr);
  }

#if 0 /* don't route via sender, instead use allow/deny settings. */
  if (!mmc && e)
    mmc = octstr_duplicate(e->fromproxy);
#endif
  /* Get the from address. */
  if (faked_sender) 
    from = octstr_duplicate(faked_sender);
  else if (xfrom != NULL)
    from = octstr_duplicate(xfrom); /* all done above. */
  else {
    /* get first recipient, set that as sender. */
    MmsEnvelopeTo *r = (e) ? gwlist_get(e->to, 0) : NULL;
    if (r)
      from = octstr_duplicate(r->rcpt);
    else 
      from = octstr_create("anon@anon");
  }     

  /* Get service code. */
  if (service_code)
    xservice_code = octstr_duplicate(service_code);
  else if (hsvc_code)
    xservice_code = octstr_duplicate(hsvc_code);
	 
  if (from)
    _mms_fixup_address(&from, NULL, NULL, 1); /* Don't normalise. */
     
  /*  Now get the data.  */
  if (octstr_str_case_compare(ctype, "application/vnd.wap.mms-message") == 0)
    m = mms_frombinary(data, from);
  else if (octstr_case_search(ctype, octstr_imm("multipart/"), 0) == 0) { /* treat it differently. */
    MIMEEntity *mime = mime_http_to_entity(reply_headers, data);
    if (mime) {
      mime_entity_destroy(me);
      me = mime;
    } 	       
  } else if (octstr_case_search(ctype, 
				octstr_imm(MBUNI_MULTIPART_TYPE), 0) == 0) { /* Mbuni multipart.*/
    List *l = octstr_split_words(data);
    MIMEEntity *mime = multipart_from_urls(l);

    if (mime) {
      mime_entity_destroy(me);
      me = mime;
    } 	       
    gwlist_destroy(l, (void *)octstr_destroy);
  } else if (octstr_case_search(ctype, octstr_imm("application/smil"), 0) == 0) {
    xmlDocPtr smil;
    xmlChar *buf = NULL;
    int bsize = 0;
    Dict *url_map = dict_create(97, (void (*)(void *))octstr_destroy);
    List *xh = http_create_empty_headers();
	  
    /* This is the hard bit: Fetch each external reference in smil, add it to message! */
    http_header_add(xh, "Content-Type", "multipart/related; "
		    "type=\"application/smil\"; start=\"<presentation>\"");
    mime_replace_headers(me,xh);
    http_destroy_headers(xh);

    /* Parse the smil as XML. */
    smil = xmlParseMemory(octstr_get_cstr(data), octstr_len(data));
    if (!smil || !smil->xmlChildrenNode) {
      *err = octstr_format("MMSBox: Error parsing SMIL response from service[%s], "
			   " msgid %s!", octstr_get_cstr(svc_name), 
			   (e && e->msgId) ? octstr_get_cstr(e->msgId) : "(none)");
      dict_destroy(url_map);
      goto done;
    }
	  
    add_msg_parts(me, smil->xmlChildrenNode, 
		  base_url, turl, type, svc_name, 
		  mmc, url_map, xmhdr, xmhdr_val);

    dict_destroy(url_map);
    /* SMIL has been modified, convert it to text, put it in. */
    xmlDocDumpFormatMemory(smil, &buf, &bsize, 1);
    xmlFreeDoc(smil);
    if (buf) {
      MIMEEntity *sm = mime_entity_create();
      List *xh = http_create_empty_headers();
      Octstr *s;
	       
      http_header_add(xh, "Content-Type", "application/smil");
      http_header_add(xh, "Content-ID", "<presentation>"); /* identify it as start element. */
      if (xmhdr)
	http_header_add(xh, octstr_get_cstr(xmhdr), octstr_get_cstr(xmhdr_val));		    
      s = octstr_create_from_data((char *)buf, bsize);
	       
      mime_replace_headers(sm, xh);
      mime_entity_set_body(sm, s);
	       
      mime_entity_add_part(me, sm);

      mime_entity_destroy(sm);
      http_destroy_headers(xh);
      octstr_destroy(s);
	       
      xmlFree(buf);
    } else {
      *err = octstr_format("MMSBox: Error writing converted SMIL "
			   "for response from service[%s], "
			   " msgid %s!", 
			   octstr_get_cstr(svc_name), 
			   (e && e->msgId) ? octstr_get_cstr(e->msgId) : "(none)");
      goto done;	       
    }	  
  } else { /* all others, make the message as-is and hope for the best! */
    List *xh = http_create_empty_headers();
	  
    if (mt_multipart) { /* if it's going to be multi-part, add some headers. */
      static int cntr = 0;
      char _fext[5] = {0}, *fext = make_file_ext(msg_url, ctype, _fext);
      Octstr *attr = octstr_format("%06d.%s", ++cntr,fext);
      Octstr *cid_header_val = octstr_format("<%S>", attr);
	  
      http_header_add(xh, "Content-ID", octstr_get_cstr(cid_header_val));
      http_header_add(xh, "Content-Location", octstr_get_cstr(attr));
	       
      octstr_destroy(attr);
      octstr_destroy(cid_header_val);
    }

    http_header_add(xh, "Content-Type", octstr_get_cstr(ctype));	  
    if (xmhdr)
      http_header_add(xh, octstr_get_cstr(xmhdr), octstr_get_cstr(xmhdr_val));		    
	  
    mime_replace_headers(me, xh);

    http_destroy_headers(xh);
    mime_entity_set_body(me, data);	  
	  
    if (mt_filter && mmc) /* filter it too. */
      mt_filter->filter(&me, msg_url, mmc);

    if (mt_multipart && 
	octstr_case_search(ctype, octstr_imm("multipart/"), 0) != 0) { /* requires multipart but this one is not, wrap it */
      MIMEEntity *xm = mime_entity_create();
      List *h1 = http_create_empty_headers();
	       
      http_header_add(h1, "Content-Type", "multipart/related");	  	       
      mime_replace_headers(xm, h1);
      mime_entity_add_part(xm, me);
	       
      http_destroy_headers(h1);
	       
      me = xm;
    }
  }
     
  /* Add some nice headers. */
  xheaders = mime_entity_headers(me);
  http_header_add(xheaders, "From", octstr_get_cstr(from));
     
  for (i = 0, n = gwlist_len(xto); i < n; i++) {
    Octstr *v;
    v = gwlist_get(xto, i);
    http_header_add(xheaders, "To", octstr_get_cstr(v));
  }
     
  if (dlr_url)
    http_header_add(xheaders, "X-Mms-Delivery-Report", "Yes");	  
  if (rr_url)
    http_header_add(xheaders, "X-Mms-Read-Report", "Yes");	  
  if (allow_adaptations)
    http_header_add(xheaders, "X-Mms-Allow-Adaptations", 
		    (octstr_str_compare(allow_adaptations, "true") == 0) ? "true" : "false");
     
  if (mclass)
    http_header_add(xheaders, "X-Mms-Message-Class", octstr_get_cstr(mclass));	  

  if (prio)
    http_header_add(xheaders, "X-Mms-Priority", octstr_get_cstr(prio));	  
     
  if (subject)
    http_header_add(xheaders, "Subject", octstr_get_cstr(subject));	  
  if (expiryt > 0) {
    Octstr *x = date_format_http(expiryt);
    http_header_add(xheaders, "X-Mms-Expiry", octstr_get_cstr(x));
    octstr_destroy(x);
  }
  mime_replace_headers(me, xheaders);
  http_destroy_headers(xheaders);

  if (me && !m) /* Set m if it hasn't been set yet. */
    m = mms_frommime(me);     
  if (!m) {
    *err = octstr_format("MMSBox: Failed to convert Mms Message from service %s!",
			 octstr_get_cstr(svc_name));
    goto done;
  }

  if (otransid) /* always add transid */
    http_header_add(hdrs, "X-Mbuni-TransactionID", octstr_get_cstr(otransid));

  if (distro) /* always add distrib */
    http_header_add(hdrs, "X-Mbuni-DistributionIndicator", 		  
		    (octstr_str_case_compare(distro, "true") == 0) ? "true" : "false");

  if (chargedparty)
    http_header_add(hdrs, "X-Mbuni-Charged-Party", octstr_get_cstr(chargedparty));			  
  if (passthro_headers && reply_headers) { /* if user wants passthro headers, get them and add them. */
    int n = gwlist_len(reply_headers);
    int i;	  
    for (i = 0; i < n; i++) {
      Octstr *h = NULL, *v = NULL;
      int j;
      http_header_get(reply_headers, i, &h, &v);
      if (h == NULL || v == NULL) 
	goto loop;
	       
      /* Check for this header in 
       *  pass thro list.
       */
      for (j = 0; j < gwlist_len(passthro_headers); j++) 
	if (octstr_case_compare(h, gwlist_get(passthro_headers, j)) == 0) {
	  http_header_add(hdrs, octstr_get_cstr(h), octstr_get_cstr(v));
	  break;
	}
    loop:
      octstr_destroy(h);
      octstr_destroy(v);
    }
  }

  /* Write to queue. */
  x = qfs->mms_queue_add(from, xto, subject, 
			 e ? e->fromproxy : NULL,
			 mmc, 
			 time(NULL), expiryt, m, NULL, 
			 xservice_code, /* Send service code as vasp id XXX - not very nice, but */
			 svc_name, 
			 dlr_url, rr_url,
			 hdrs,
			 (dlr_url != NULL), 
			 octstr_get_cstr(qdir), 
			 "MMSBox",
			 NULL);
  mms_info(0, "mmsbox", NULL,"MMSBox: Queued message from service [%s], [transid [%s]: %s",
	   octstr_get_cstr(svc_name), 
	   otransid ? octstr_get_cstr(otransid) : "",
	   octstr_get_cstr(x));
  *err = x;
  res = 0;
 done:

  octstr_destroy(dlr_url);     
  octstr_destroy(rr_url);
  octstr_destroy(mclass);
  octstr_destroy(prio);     
  octstr_destroy(mmc);     
  octstr_destroy(allow_adaptations);
  octstr_destroy(from);
  octstr_destroy(xfrom);
  octstr_destroy(subject);
  octstr_destroy(otransid);
  octstr_destroy(turl);
  octstr_destroy(chargedparty);
     
  octstr_destroy(xmhdr);
  octstr_destroy(xmhdr_val);
  if (me)
    mime_entity_destroy(me);
     
  mms_destroy(m);
  gwlist_destroy(xto, (void *)octstr_destroy);
  http_destroy_headers(hdrs);     
  octstr_destroy(xservice_code);
  octstr_destroy(hsvc_code);

  return res;
}


static SendMmsUser *auth_user(Octstr *user, Octstr *pass)
{
  int i, n;
  SendMmsUser *u = NULL;

  if (!user || !pass)
    return NULL;
  for (i = 0, n = gwlist_len(sendmms_users); i < n; i++) 	  
    if ((u = gwlist_get(sendmms_users, i)) != NULL && 
	octstr_compare(u->user, user) == 0 && 
	octstr_compare(u->pass, pass) == 0)
      return u;
  return NULL;
}

static void dispatch_sendmms_recv(List *rl)
{
  MmsBoxHTTPClientInfo *h;
     
  while ((h = gwlist_consume(rl)) != NULL) {
    SendMmsUser *u = NULL;
    List *hh = http_create_empty_headers();
    Octstr *username;
    Octstr *password;
    Octstr *err = NULL;
    List *cgivar_ctypes = NULL; 
    char *etype = NULL;

    int res = 0, tparse = parse_cgivars(h->headers, h->body, &h->cgivars, &cgivar_ctypes);
	  
    username = http_cgi_variable(h->cgivars, "username");
    password = http_cgi_variable(h->cgivars, "password");
    if (username)
      octstr_strip_blanks(username);
    if (password)
      octstr_strip_blanks(password);
    if ((u = auth_user(username, password)) != NULL && 
	is_allowed_ip(sendmms_port.allow_ip, sendmms_port.deny_ip, h->ip)) {
      Octstr *data, *external_data = NULL, *ctype = NULL, *mmc, *to, *from, *dlr_url;
      Octstr *rr_url, *allow_adaptations, *subject = NULL;   
      List *lto = NULL, *rh = http_create_empty_headers();
      Octstr *rb = NULL, *base_url;
      int i, n, skip_ctype = 0;
      Octstr *vasid = http_cgi_variable(h->cgivars, "vasid");
      Octstr *service_code = http_cgi_variable(h->cgivars, "servicecode");
      Octstr *mclass = http_cgi_variable(h->cgivars, "mclass");
      Octstr *prio = http_cgi_variable(h->cgivars, "priority");
      Octstr *distro = http_cgi_variable(h->cgivars, "distribution");
      Octstr *send_type = http_cgi_variable(h->cgivars, "mms-direction");
      Octstr *sheader = http_cgi_variable(h->cgivars, "extra-content-header");
      Octstr *validity = http_cgi_variable(h->cgivars, "validityperiod"); 
      Octstr *chargedparty = http_cgi_variable(h->cgivars, "charged-party"); 
      Octstr *data_url = NULL;
	       
      dlr_url =  http_cgi_variable(h->cgivars, "dlr-url");
      rr_url  =  http_cgi_variable(h->cgivars, "rr-url");	       
      allow_adaptations  =  http_cgi_variable(h->cgivars, "allow-adaptations");        

	       
      if ((mmc =  http_cgi_variable(h->cgivars, "mmsc")) == NULL) {
	mmc = octstr_duplicate(u->mmsc); /* could still be NULL */
      }
      subject =  http_cgi_variable(h->cgivars, "subject");
	       
      if ((base_url = http_cgi_variable(h->cgivars, "base-url")) == NULL)
	base_url = octstr_create("http://localhost");
      else 
	base_url = octstr_duplicate(base_url); /* because we need to delete it below. */

      /* Now get the data. */
      if ((data = http_cgi_variable(h->cgivars, "text")) != NULL) { /* text. */
	Octstr *charset = http_cgi_variable(h->cgivars, "charset");
		    
	ctype = octstr_format("text/plain");
	if (charset) 
	  octstr_format_append(ctype, "; charset=%S", charset);
      } else  if ((data = http_cgi_variable(h->cgivars, "smil")) != NULL)  /* smil. */
	ctype = octstr_create("application/smil");
      else if ((data = http_cgi_variable(h->cgivars, "content-url")) != NULL) {  /* arbitrary content. */
	List *reqh = http_create_empty_headers(), *rph = NULL;	
	Octstr *reply = NULL, *params = NULL;
		    
	http_header_add(reqh, "User-Agent", MM_NAME "/" VERSION);	       
		    
	if (mmsbox_url_fetch_content(HTTP_METHOD_GET, data, reqh, NULL, &rph, &reply) == HTTP_OK)
	  get_content_type(rph, &ctype, &params);
	else 
	  rb = octstr_format("failed to fetch content from url [%S]!", data);		    
	base_url = url_path_prefix(data, URL_TYPE);

	data_url = octstr_duplicate(data); /* the URL of the message. */
	data = external_data = reply; /* We record it separately for deletion below. */

	http_destroy_headers(reqh);
	http_destroy_headers(rh);
	rh = rph ? rph : http_create_empty_headers(); /* replace as real reply headers. */
	skip_ctype = 1;
	octstr_destroy(params);
      } else if ((data = http_cgi_variable(h->cgivars, "content")) != NULL) { /* any content. */		    
	Octstr *_xctype = NULL; /* ... because cgi var stuff is destroyed elsewhere, we must dup it !! */
		    
	/* If the user sent us a content element, then they should have
	 * sent us its type either as part of the HTTP POST (multipart/form-data)
	 * or as CGI VAR called content_type 
	 */
	if ((_xctype = http_cgi_variable(h->cgivars, "content_type")) == NULL)
	  if (cgivar_ctypes) 
	    _xctype = http_cgi_variable(cgivar_ctypes, "content");  
	if (_xctype) 
	  ctype = octstr_duplicate(_xctype); 
	else 
	  ctype = octstr_create("application/octet-stream");
      } else if (h->body && tparse != 0) { /* if all above fails, 
					      use send-mms msg body if any. */
	data = octstr_duplicate(h->body);
	ctype = http_header_value(h->headers, octstr_imm("Content-Type"));
      } else
	rb = octstr_create("Missing content");
	       	  
	       
      if ((to =  http_cgi_variable(h->cgivars, "to")) == NULL)
	rb = octstr_create("Missing recipient!");
      else 
	lto = octstr_split_words(to);
	       
      /* Build the fake reply headers. */
      if (ctype && !skip_ctype)
	http_header_add(rh, "Content-Type", octstr_get_cstr(ctype));

      if ((from = http_cgi_variable(h->cgivars, "from")) != NULL) {
	from = octstr_duplicate(from);
	_mms_fixup_address(&from, NULL, NULL, 1);
	if (from) 
	  HTTP_REPLACE_HEADER(rh, "X-Mbuni-From", octstr_get_cstr(from));
	octstr_destroy(from);
      } else if (!u->faked_sender)
	rb = octstr_create("Missing Sender address");
	       
      if (lto) {
	for (i = 0, n = gwlist_len(lto); i < n; i++)  {
	  Octstr *x = gwlist_get(lto, i);
	  http_header_add(rh, "X-Mbuni-To", octstr_get_cstr(x));		 
	}
	gwlist_destroy(lto, (gwlist_item_destructor_t *)octstr_destroy);
      }
      if (dlr_url) 
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-DLR-Url", octstr_get_cstr(dlr_url));

      if (rr_url) 
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-RR-Url", octstr_get_cstr(rr_url));

      if (allow_adaptations)
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-Allow-Adaptations", 
			    (octstr_str_compare(allow_adaptations, "1") == 0) ? 
			    "true" : "false");  	       
      if (mmc) 
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-MMSC", octstr_get_cstr(mmc));

      if (subject) 
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-Subject", octstr_get_cstr(subject));
	       	    
      if (mclass)
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-MessageClass", octstr_get_cstr(mclass));
      if (prio)
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-Priority", octstr_get_cstr(prio));
      if (distro)
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-DistributionIndicator", octstr_get_cstr(distro));

      if (sheader)
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-Content-Header", octstr_get_cstr(sheader));

      if (chargedparty)
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-Charged-Party", octstr_get_cstr(chargedparty));
	       
      if (validity) {
	int diff = atoi(octstr_get_cstr(validity));
	time_t t = time(NULL) + (diff *60);
	Octstr *xt = date_format_http(t);
		    
	HTTP_REPLACE_HEADER(rh, "X-Mbuni-Expiry", octstr_get_cstr(xt));

	octstr_destroy(xt);
      }

      /* Requests to make_and_queue below can block, but for now we don't care. */
      if (ctype && data && !rb) { /* only send if no error. */
	int send_as_incoming = (send_type && 
				octstr_str_case_compare(send_type, "mo") == 0);
	Octstr *transid = NULL;
	if (!send_as_incoming) { /* for outgoing, track TransactionID for DLR. */
	  transid = mms_maketransid(NULL, octstr_imm(MM_NAME));
	  HTTP_REPLACE_HEADER(rh, "X-Mbuni-TransactionID", octstr_get_cstr(transid));
	}
	res = make_and_queue_msg(data, ctype, rh, 
				 data_url ? data_url : base_url,
				 base_url, URL_TYPE, NULL,
				 vasid ? vasid : octstr_imm("sendmms-user"), 
				 u->faked_sender, service_code,
				 1, NULL, 
				 send_as_incoming ? incoming_qdir : outgoing_qdir,
				 &err);
	if (res < 0)
	  rb = octstr_format("Error in message conversion: %S", err);
	else 
	  rb = octstr_format("Accepted: %S", /* repeat transid. */
			     send_as_incoming ? err : transid);
	octstr_destroy(transid);
      }  else if (!rb)
	rb = octstr_create("Failed to send message");	       
      http_send_reply(h->client, (res == 0) ? HTTP_OK : HTTP_BAD_REQUEST, hh, 
		      rb ? rb : octstr_imm("Sent"));	       
      mms_info(0, "mmsbox", NULL,"MMSBox.mmssend: u=%s, %s [%s]", 
	       u ? octstr_get_cstr(u->user) : "none", 
	       (res == 0) ? "Queued" : "Not Queued", 
	       rb ? octstr_get_cstr(rb) : "");
	       
      http_destroy_headers(rh);	       
      octstr_destroy(ctype);
      octstr_destroy(rb);
      octstr_destroy(base_url);
      octstr_destroy(data_url);
      octstr_destroy(external_data);
    } else {
      http_send_reply(h->client, HTTP_UNAUTHORIZED, hh, 
		      octstr_imm("Authentication failed"));
      err = octstr_format("MMSBox: SendMMS, Authentication failed, "
			  "username=%s, password=%s!",
			  username ? octstr_get_cstr(username) : "(null)",
			  password ? octstr_get_cstr(password) : "(null)");
      etype = "auth";
    }
	  
    if (err && res != 0) 
      mms_error_ex(etype, 0,  "mmsbox", NULL, "%s", octstr_get_cstr(err));	 
    octstr_destroy(err);
    /* Free the ip and other stuff here. */	  	

    http_destroy_cgiargs(cgivar_ctypes);

    http_destroy_headers(hh);	  
    free_mmsbox_http_clientInfo(h, 1);
  }
}


static void sendmms_func(void *unused)
{
  MmsBoxHTTPClientInfo h = {NULL};
  int i;
  List *rl = gwlist_create();


  gwlist_add_producer(rl);
     
  for (i = 0; i<maxthreads; i++)
    gwthread_create((gwthread_func_t *)dispatch_sendmms_recv, rl);

     
  while (!rstop  &&
	 (h.client = http_accept_request(sendmms_port.port, 
					 &h.ip, &h.url, &h.headers, &h.body, &h.cgivars)) != NULL) {
    MmsBoxHTTPClientInfo *hx = gw_malloc(sizeof hx[0]); 

    *hx = h;		    
    hx->ua = http_header_value(h.headers, octstr_imm("User-Agent"));
	  
    gwlist_produce(rl, hx);
  }
    
  debug("proxy", 0, "MMSBox: MMS-Send Interface shutting down ... ");          
  gwlist_remove_producer(rl);
  gwthread_join_every((void *)dispatch_sendmms_recv);
  gwlist_destroy(rl, NULL);
  debug("proxy", 0, "MMSBox: MMS-Send Interface shutdown complete. ");          
     
}

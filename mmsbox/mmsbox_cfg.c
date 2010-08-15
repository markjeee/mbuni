/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBox CFG: MMBox configuration and misc. functions
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
#include "mmsbox_cfg.h"
#include "mms_queue.h"
#include "mmsbox_resolve_shell.h"

#define WAIT_TIME 0.2

List   *sendmms_users = NULL; /* list of SendMmsUser structs */
List   *mms_services = NULL;  /* list of MMS Services */
Octstr *incoming_qdir, *outgoing_qdir, *dlr_dir;
long   svc_maxsendattempts, maxsendattempts, mmsbox_send_back_off, default_msgexpiry, max_msgexpiry = -1;
long   maxthreads = 0;
double queue_interval = -1;
Octstr *unified_prefix = NULL;
List   *strip_prefixes = NULL;
Octstr *sendmail_cmd = NULL;
Octstr *myhostname = NULL;

int mt_multipart = 0;
MmsQueueHandlerFuncs *qfs; /* queue functions. */
MmsBoxResolverFuncStruct *rfs; /* resolver functions. */
void *rfs_data;
Octstr *rfs_settings; 

MmsBoxCdrFuncStruct *cdrfs;

static mCfg *cfg; /* Config. */
static Dict *mmscs = NULL; /* MMSC's indexed by ID. */
static List *mmsc_del_list = NULL; /* List of items to be deleted. */

/* admin handler variables. */
static long admin_port;
static long admin_thread = -1; 
static Octstr *admin_pass = NULL;
static Octstr *admin_allow_ip = NULL;
static Octstr *admin_deny_ip = NULL;

static gwthread_func_t *mmsc_receiver_func;

struct SendMmsPortInfo sendmms_port = {-1};

struct MmsBoxMTfilter *mt_filter = NULL;

static void free_mmsc_struct (MmscGrp *m);

static void admin_handler(void *unused);

int mms_load_mmsbox_settings(Octstr *fname, gwthread_func_t *mmsc_handler_func)
{     
     mCfgGrp *grp;
     mCfgGrp *cgrp;
     Octstr *gdir = NULL, *s;

     int send_port_ssl = 0, admin_port_ssl = 0;
     List *l;
     int i, n, xx;
     void *catchall = NULL, *x;


     if ((cfg = mms_cfg_read(fname)) == NULL) {
	  mms_error(0, "mmsbox", NULL, "Couldn't read configuration  from '%s'.", octstr_get_cstr(fname));	  
	  return -1;
     }
     
     grp = mms_cfg_get_single(cfg, octstr_imm("mbuni"));
     cgrp = mms_cfg_get_single(cfg, octstr_imm("core"));
     
     if (grp == NULL) {
	  mms_error(0, "mmsbox", NULL, "Missing required group `mbuni' in config file!");
	  return -1;
     }

     mms_load_core_settings(cfg, cgrp);
     
     sendmms_users = gwlist_create();
     mms_services = gwlist_create();
     mmscs = dict_create(101, NULL);     

     mmsc_del_list = gwlist_create();

     if ((x = _mms_load_module(cfg, grp, "event-logger-module", "event_logger", NULL)) != NULL) {
	  Octstr *s = mms_cfg_get(cfg, grp, octstr_imm("event-logger-module"));
	  
	  if (mms_event_logger_init(x, s) != 0)
	       panic(0, "Mmsbox: Failed to initialise event logger module!");
	  octstr_destroy(s);
     }

     if (mms_cfg_get_int(cfg, grp, 
			 octstr_imm("maximum-send-attempts"), &maxsendattempts) < 0)
	  maxsendattempts = MAXQTRIES;

     if (mms_cfg_get_int(cfg, grp, 
			 octstr_imm("mmsbox-maximum-request-attempts"), &svc_maxsendattempts) < 0)
	  svc_maxsendattempts = maxsendattempts;
     
     if (mms_cfg_get_int(cfg, grp, 
		     octstr_imm("send-attempt-back-off"), &mmsbox_send_back_off) == -1)
	  mmsbox_send_back_off = BACKOFF_FACTOR;

     if (mms_cfg_get_int(cfg, grp, octstr_imm("default-message-expiry"), &default_msgexpiry) == -1)
	  default_msgexpiry = DEFAULT_EXPIRE;

     if (mms_cfg_get_int(cfg, grp, octstr_imm("max-message-expiry"), &max_msgexpiry) == -1)
	  max_msgexpiry = -1;

     if (mms_cfg_get_int(cfg, grp, octstr_imm("max-send-threads"), &maxthreads) < 0 || 
	 maxthreads < 1)
	  maxthreads = 10;
     
     s = mms_cfg_get(cfg, grp, octstr_imm("queue-run-interval"));

     if (s) {
	  queue_interval = atof(octstr_get_cstr(s));
	  octstr_destroy(s);
     }
     if (queue_interval <= 0)
	  queue_interval = QUEUERUN_INTERVAL;
     
     if ((sendmail_cmd = mms_cfg_get(cfg, grp, octstr_imm("send-mail-prog"))) == NULL)
	  sendmail_cmd = octstr_imm("/usr/sbin/sendmail -f '%f' '%t'");

     if ((myhostname = mms_cfg_get(cfg, grp, octstr_imm("hostname"))) == NULL)
	  myhostname = octstr_imm("localhost");
     

     gdir = mms_cfg_get(cfg, grp, octstr_imm("storage-directory"));     

     if (gdir == NULL)
	  gdir = octstr_imm(".");
     
     if (mkdir(octstr_get_cstr(gdir), 
	       S_IRWXU|S_IRWXG) < 0 && 
	 errno != EEXIST)
	  panic(0, "Failed to create MMSBox storage directory: %s - %s!",
		octstr_get_cstr(gdir), strerror(errno));
     
     if ((qfs = _mms_load_module(cfg, grp, "queue-manager-module", "qfuncs", NULL)) == NULL) {
	  qfs = &default_qfuncs; /* default queue handler. */
	  qfs->mms_init_queue_module(gdir, 
				     octstr_get_cstr(gdir), 
				     (2 + 1)*maxthreads); /* We expect 2 each for each mmsbox thread, 
							   * one each for each bearerbox thread. 
							   */
     } else {
	  Octstr *s = _mms_cfg_getx(cfg, grp, octstr_imm("queue-module-init-data"));
	  if (qfs->mms_init_queue_module(s,
					 octstr_get_cstr(gdir), 
					 (2+1)*maxthreads) != 0)
	       panic(0, "failed to initialise queue module, with data: %s",
		     octstr_get_cstr(s));
	  octstr_destroy(s);
     }


     if ((cdrfs = _mms_load_module(cfg, grp, "mmsbox-cdr-module", "cdr_funcs", NULL)) == NULL)
	  cdrfs = &mmsbox_cdrfuncs; /* default CDR handler. */

     if ((s = mms_cfg_get(cfg, grp, octstr_imm("mmsbox-cdr-module-parameters"))) == NULL) 
	  s = octstr_format("%S/mmsbox-cdr.asc", gdir);

     if (cdrfs->init(octstr_get_cstr(s)) != 0)
	  panic(0, "Failed to initialised CDR module: %s", strerror(errno));
     octstr_destroy(s);


     if ((incoming_qdir = qfs->mms_init_queue_dir("mmsbox_incoming", &xx)) == NULL ||
	 xx != 0)
	  panic(0, "Failed to initialise incoming mmsbox queue directory: %s - %s!", 
		octstr_get_cstr(incoming_qdir), strerror(errno));

     if ((outgoing_qdir = qfs->mms_init_queue_dir("mmsbox_outgoing", &xx)) == NULL ||
	 xx != 0)
	  panic(0, "Failed to initialise outgoing mmsbox queue directory: %s - %s!", 
		octstr_get_cstr(outgoing_qdir), strerror(errno));

     /* XXX still uses old-style file storage. */
     if (qfs != &default_qfuncs) 
	  default_qfuncs.mms_init_queue_module(gdir, 	
					       octstr_get_cstr(gdir), 
					       maxthreads);
     if ((dlr_dir = default_qfuncs.mms_init_queue_dir("mmsbox_dlr", &xx)) == NULL ||
	 xx != 0) 
	  panic(0, "Failed to initialise dlr storage directory: %s - %s!", 
		octstr_get_cstr(dlr_dir), strerror(errno));
     
          
     unified_prefix = _mms_cfg_getx(cfg, grp, octstr_imm("unified-prefix"));  
     
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("strip-prefixes"))) != NULL) {
	  strip_prefixes = octstr_split(s, octstr_imm(";"));
	  octstr_destroy(s);
     } else 
	  strip_prefixes = NULL;

     mms_cfg_get_int(cfg, grp, octstr_imm("sendmms-port"), &sendmms_port.port);
#ifdef HAVE_LIBSSL
     mms_cfg_get_bool(cfg, grp, octstr_imm("sendmms-port-ssl"), &send_port_ssl);
#endif
     
     if (sendmms_port.port > 0 && 
	 http_open_port(sendmms_port.port, send_port_ssl) < 0) {
	  mms_error(0,  "mmsbox", NULL, "Failed to start sendmms HTTP server on %ld: %s!",
		    sendmms_port.port,
		    strerror(errno));
	  sendmms_port.port = -1;
     }
     
     sendmms_port.allow_ip = mms_cfg_get(cfg, grp, octstr_imm("allow-ip"));          
     sendmms_port.deny_ip =  mms_cfg_get(cfg, grp, octstr_imm("deny-ip"));          

     /* load the filter if any. */
     if ((mt_filter = _mms_load_module(cfg, grp, "mmsbox-mt-filter-library", "mmsbox_mt_filter", NULL)) != NULL)
	  mms_info(0, "mmsbox", NULL, "Loaded MT Filter [%s]", mt_filter->name);	      

     mms_cfg_get_bool(cfg, grp, octstr_imm("mmsbox-mt-always-multipart"), &mt_multipart);

     /* load the resolver module. */
     if ((rfs = _mms_load_module(cfg, grp, "resolver-library", "mmsbox_resolvefuncs", &mmsbox_resolvefuncs_shell)) == NULL)
	  rfs = &mmsbox_resolvefuncs;

     rfs_settings = _mms_cfg_getx(cfg, grp,  octstr_imm("resolver-module-parameters"));
     rfs_data = rfs->mmsbox_resolvermodule_init(rfs_settings ? octstr_get_cstr(rfs_settings) : NULL);
     
     /* Now get sendmms users. */     
     l = mms_cfg_get_multi(cfg, octstr_imm("send-mms-user"));
     for (i = 0, n = gwlist_len(l); i < n; i++) {
	  mCfgGrp *x = gwlist_get(l, i);
	  SendMmsUser *u = gw_malloc(sizeof *u);

	  memset(u, 0, sizeof *u);
	  
	  u->user = _mms_cfg_getx(cfg, x, octstr_imm("username"));
	  u->pass = _mms_cfg_getx(cfg, x, octstr_imm("password"));
	  u->faked_sender = mms_cfg_get(cfg, x, octstr_imm("faked-sender"));	  	  
	  u->dlr_url = _mms_cfg_getx(cfg, x, octstr_imm("delivery-report-url"));	  	  
	  u->rr_url = _mms_cfg_getx(cfg, x, octstr_imm("read-report-url"));	  	  
	  u->mmsc = mms_cfg_get(cfg, x, octstr_imm("mmsc"));
	  gwlist_append(sendmms_users, u);

	  mms_cfg_destroy_grp(cfg, x);
     }    
     gwlist_destroy(l, NULL);
     
     mmsc_receiver_func = mmsc_handler_func; /* save it. */

     /* Start MMSCs. */
     l = mms_cfg_get_multi(cfg, octstr_imm("mmsc"));
     for (i = 0, n = gwlist_len(l); i < n; i++) {
	  List *errors = gwlist_create();
	  List *warnings = gwlist_create();
	  Octstr *x;
	  mCfgGrp *xgrp = gwlist_get(l, i);

	  start_mmsc_from_conf(cfg, xgrp, mmsc_handler_func, errors, warnings);
	  
	  while ((x = gwlist_extract_first(errors)) != NULL) {
	       mms_error(0,  "mmsbox", NULL, "%s", octstr_get_cstr(x));
	       octstr_destroy(x);
	  }

	  while ((x = gwlist_extract_first(warnings)) != NULL) {
	       mms_warning(0,  "mmsbox", NULL, "%s", octstr_get_cstr(x));
	       octstr_destroy(x);
	  }
	  gwlist_destroy(errors, NULL);
	  gwlist_destroy(warnings, NULL);

	  mms_cfg_destroy_grp(cfg, xgrp);
     }
     
     gwlist_destroy(l, NULL);


     l = mms_cfg_get_multi(cfg, octstr_imm("mms-service"));     
     for (i = 0, n = gwlist_len(l); i < n; i++) {
	  mCfgGrp *x = gwlist_get(l, i);
	  MmsService *m = gw_malloc(sizeof *m);
	  Octstr *s;
	  
	  m->name = _mms_cfg_getx(cfg, x, octstr_imm("name"));
	  if ((m->url = mms_cfg_get(cfg, x, octstr_imm("get-url"))) != NULL)
	       m->type = TRANS_TYPE_GET_URL;
	  else if ((m->url = mms_cfg_get(cfg, x, octstr_imm("post-url"))) != NULL)
	       m->type = TRANS_TYPE_POST_URL;
	  else if ((m->url = mms_cfg_get(cfg, x, octstr_imm("file"))) != NULL)
	       m->type = TRANS_TYPE_FILE;
	  else if ((m->url = mms_cfg_get(cfg, x, octstr_imm("exec"))) != NULL)
	       m->type = TRANS_TYPE_EXEC;
	  else if ((m->url = mms_cfg_get(cfg, x, octstr_imm("text"))) != NULL)
	       m->type = TRANS_TYPE_TEXT;
	  else 
	       panic(0, "MMSBox: Service [%s] has no url!", octstr_get_cstr(m->name));
	  
	  m->faked_sender = mms_cfg_get(cfg, x, octstr_imm("faked-sender"));	  	  
	  
	  m->isdefault = 0;
	  mms_cfg_get_bool(cfg, x, octstr_imm("catch-all"), &m->isdefault);     
	  if (m->isdefault) {
	       if (catchall)
		    mms_warning(0, "mmsbox", NULL, "Multiple default mms services defined!");	       
	       catchall = m;
	  }

	  if (mms_cfg_get_bool(cfg, x, octstr_imm("omit-empty"), &m->omitempty) < 0)
	       m->omitempty = 0;
	  if (mms_cfg_get_bool(cfg, x, octstr_imm("suppress-reply"), &m->noreply) < 0)
	       m->noreply = 0;
	  mms_cfg_get_bool(cfg, x, octstr_imm("accept-x-mbuni-headers"), &m->accept_x_headers); 

	  if ((s = mms_cfg_get(cfg, x, octstr_imm("pass-thro-headers"))) != NULL) {
	       m->passthro_headers = octstr_split(s, octstr_imm(","));
	       octstr_destroy(s);
	  } else 
	       m->passthro_headers = NULL;

	  mms_cfg_get_bool(cfg, x, octstr_imm("assume-plain-text"), &m->assume_plain_text);

	  if ((s = mms_cfg_get(cfg, x, octstr_imm("accepted-mmscs"))) != NULL) {
	       m->allowed_mmscs = octstr_split(s, octstr_imm(";"));
	       octstr_destroy(s);
	  } else 
	       m->allowed_mmscs = NULL; /* means allow all. */

	  if ((s = mms_cfg_get(cfg, x, octstr_imm("denied-mmscs"))) != NULL) {
	       m->denied_mmscs = octstr_split(s, octstr_imm(";"));
	       octstr_destroy(s);
	  } else 
	       m->denied_mmscs = NULL; /* means allow all. */

	  m->allowed_receiver_prefix = mms_cfg_get(cfg, x, octstr_imm("allowed-receiver-prefix"));
	  m->denied_receiver_prefix = mms_cfg_get(cfg, x, octstr_imm("denied-receiver-prefix"));
	 
	  /* Get key words. Start with aliases to make life easier. */
	  if ((s = mms_cfg_get(cfg, x, octstr_imm("aliases"))) != NULL) {
	       m->keywords = octstr_split(s, octstr_imm(";"));
	       octstr_destroy(s);
	  } else 
	       m->keywords = gwlist_create();
	  
	  s = mms_cfg_get(cfg, x, octstr_imm("keyword"));
	  if (!s) 
	       panic(0, "MMSBox: Service [%s] has no keyword!", octstr_get_cstr(m->name));
	  else 
	       gwlist_append(m->keywords, s);
	  
	  if ((s = mms_cfg_get(cfg, x, octstr_imm("http-post-parameters"))) != NULL) {
	       List *r = octstr_split(s, octstr_imm("&"));
	       int i, n;
	       m->params = gwlist_create();
	       if (m->type != TRANS_TYPE_POST_URL)
		    mms_warning(0, "mmsbox", NULL,"Service [%s] specifies HTTP Post parameters "
			    "without specifying post-url type/url!", octstr_get_cstr(m->name));
	       for (i = 0, n = gwlist_len(r); i < n; i++) {
		    Octstr *y = gwlist_get(r, i);
		    int ii = octstr_search_char(y, '=', 0);
		    if (ii < 0)
			 ii = octstr_len(y);
		    
		    if (ii > 0) {
			 MmsServiceUrlParam *p = gw_malloc(sizeof *p);			 
			 int ch;
			 p->name = octstr_copy(y, 0, ii);
			 p->value = NULL;

			 if (octstr_get_char(y, ii+1) == '%') {
			      switch(ch = octstr_get_char(y, ii+2)) {
			      case 'a':
				   p->type = AUDIO_PART; break;
			      case 'b':
				   p->type = WHOLE_BINARY; break;
			      case 'i':
				   p->type = IMAGE_PART; break;
			      case 'v':
				   p->type = VIDEO_PART; break;				   
			      case 't':
				   p->type = TEXT_PART; break;				   
			      case 's':
				   p->type = SMIL_PART; break;				   
			      case 'o':
				   p->type = OTHER_PART; break;				   
			      case 'z':
				   p->type = ANY_PART; break;				   
			      case 'k':
				p->type = KEYWORD_PART; break;
			      case '%':
				   p->type = NO_PART; break;
			      default:
				   mms_warning(0, "mmsbox", NULL, "Unknown conversion character %c "
					   "in http-post-parameters. Service [%s]!",
					   ch, octstr_get_cstr(m->name));
				   p->type = NO_PART;
				   break;
			      }
			      p->value = octstr_copy(y, ii+3, octstr_len(y));
			 } else { /* No conversion spec. */
			      p->type = NO_PART; 
			      p->value = octstr_copy(y, ii+1, octstr_len(y));
			 }
			 gwlist_append(m->params, p);
		    } else 
			 mms_warning(0, "mmsbox", NULL, "Missing http-post-parameter name? Service [%s]!",
				 octstr_get_cstr(m->name));
	       }
	       gwlist_destroy(r, (gwlist_item_destructor_t *)octstr_destroy);
	       octstr_destroy(s);
	  } else 
	       m->params = NULL;

	  m->service_code = mms_cfg_get(cfg, x, octstr_imm("service-code"));
	  m->special_header = mms_cfg_get(cfg, x, octstr_imm("extra-reply-content-header"));
	  gwlist_append(mms_services, m);

	  mms_cfg_destroy_grp(cfg, x);
     }

     /* Finally load admin-port config and start the thingie */

     mms_cfg_get_int(cfg, grp, octstr_imm("mmsbox-admin-port"), &admin_port);
#ifdef HAVE_LIBSSL
     mms_cfg_get_bool(cfg, grp, octstr_imm("admin-port-ssl"), &admin_port_ssl);
#endif
     admin_pass = mms_cfg_get(cfg, grp, octstr_imm("admin-password"));
     
     admin_allow_ip = mms_cfg_get(cfg, grp, octstr_imm("admin-allow-ip"));
     admin_deny_ip = mms_cfg_get(cfg, grp, octstr_imm("admin-deny-ip"));
     
     if (admin_port > 0 &&
	 http_open_port(admin_port, admin_port_ssl)< 0)
	  mms_error(0,  "mmsbox", NULL, "Failed to start admin server on port %d: %s",
		(int)admin_port, strerror(errno));
     else if (admin_port > 0 &&
	      (admin_thread = gwthread_create((gwthread_func_t *)admin_handler, NULL)) < 0) {
	  mms_error(0,  "mmsbox", NULL, "Failed to start admin server thread: %s",
		strerror(errno));
	  http_close_port(admin_port);
     } else if (admin_pass == NULL) 
	  mms_warning(0,  "mmsbox", NULL, "Empty or no password supplied for admin port. All requests will be allowed!");
          
     gwlist_destroy(l, NULL);
     octstr_destroy(gdir);

     mms_cfg_destroy_grp(cfg, cgrp);
     mms_cfg_destroy_grp(cfg, grp);
     return 0;
}

/* do nothing func: Vital it returns 0! */
static int do_nothing_func (void) {return 0;}

static MmsBoxMmscFuncs dummy_mmsc_funcs = {
     (void *)do_nothing_func,
     (void *)do_nothing_func,
     (void *)do_nothing_func
};

#define ERROR(fmt,...) do {if (errors) gwlist_append(errors, octstr_format((fmt), ##__VA_ARGS__));} while(0)
#define WARNING(fmt,...) do {if (warnings) gwlist_append(warnings, octstr_format((fmt), ##__VA_ARGS__));}while(0)



static void mmsbox_stop_mmsc_conn_real(MmscGrp *mmc)
{
     
     if (mmc->type == CUSTOM_MMSC && mmc->fns 
	 && mmc->custom_started) {
	  mmc->fns->stop_conn(mmc->data);
	  mmc->custom_started = 0;
     } else if (mmc->incoming.port > 0) {
	  http_close_port(mmc->incoming.port);    
	  if (mmc->threadid >= 0)
	       gwthread_join(mmc->threadid);
	  mmc->threadid = -1;
     }
     mms_info(0,  "mmsbox", NULL,"Shutdown for mmsc [%s] complete", octstr_get_cstr(mmc->id));
}


static void mmsbox_start_mmsc_conn(MmscGrp *m, gwthread_func_t *mmsc_handler_func,
				   List *errors, List *warnings) 
{
     if (m->type == CUSTOM_MMSC) {
	  if (m->fns == NULL ||
	      m->fns->start_conn(m, qfs, unified_prefix, strip_prefixes, &m->data) != 0) {
	       WARNING("MMSBox: Failed to start custom MMSC [%s]", octstr_get_cstr(m->id));
	       m->custom_started = 0;
	  } else 
	       m->custom_started = 1;
     } else {
	  if (m->incoming.port > 0 && 
	      http_open_port(m->incoming.port, m->incoming.ssl) < 0) {
	       WARNING("MMSBox: Failed to start HTTP server on receive port for "
		       " MMSC %s, port %ld: %s!",
		       octstr_get_cstr(m->id), m->incoming.port,
		       strerror(errno));
	       m->incoming.port = 0; /* so we don't listen on it. */
	  }
	  
	  if (mmsc_handler_func && 
	      m->incoming.port > 0) { /* Only start threads if func passed and ... */
	       if ((m->threadid = gwthread_create(mmsc_handler_func, m)) < 0)
		    ERROR("MMSBox: Failed to start MMSC handler thread for MMSC[%s]: %s!",
					 octstr_get_cstr(m->id), strerror(errno));
	  } else 
	       m->threadid = -1;
     }

     mms_info(0,  "mmsbox", NULL,"Startup for mmsc [%s] complete", octstr_get_cstr(m->id));
}



MmscGrp *start_mmsc_from_conf(mCfg *cfg, mCfgGrp *x, gwthread_func_t *mmsc_handler_func, 
			 List *warnings, List *errors)
{

     MmscGrp *m = gw_malloc(sizeof *m);
     int ssl = 0;
     Octstr *type, *tmp;
     Octstr *xver;
     Octstr *s;
     
     memset(m, 0, sizeof *m);
          
     m->id = _mms_cfg_getx(cfg, x, octstr_imm("id"));
     if (octstr_len(m->id) < 1) {
	  ERROR("mmsbox.mmsc_config: Missing required field value `id' in config file!");
	  octstr_destroy(m->id);
	  gw_free(m);

	  return NULL;
     }

     m->group_id = mms_cfg_get(cfg, x, octstr_imm("group-id"));
     if (m->group_id == NULL) 
	  m->group_id = octstr_duplicate(m->id); 

     m->vasp_id = mms_cfg_get(cfg, x, octstr_imm("vasp-id"));
     if (m->vasp_id == NULL)
          m->vasp_id = octstr_duplicate(m->id);
     
     m->mmsc_url = _mms_cfg_getx(cfg, x, octstr_imm("mmsc-url"));
     
     m->allowed_prefix = mms_cfg_get(cfg, x, octstr_imm("allowed-prefix"));
     m->denied_prefix = mms_cfg_get(cfg, x, octstr_imm("denied-prefix"));
     
     m->allowed_sender_prefix = mms_cfg_get(cfg, x, octstr_imm("allowed-sender-prefix"));
     m->denied_sender_prefix = mms_cfg_get(cfg, x, octstr_imm("denied-sender-prefix"));
     
     m->incoming.allow_ip = mms_cfg_get(cfg, x, octstr_imm("allow-ip"));          
     m->incoming.deny_ip =  mms_cfg_get(cfg, x, octstr_imm("deny-ip"));          
     
     mms_info(0,  "mmsbox", NULL,"Loaded MMSC[%s], allow=[%s], deny=[%s] group_id=[%s]", 
	  octstr_get_cstr(m->id),
	  octstr_get_cstr(m->incoming.allow_ip), 
	  octstr_get_cstr(m->incoming.deny_ip), 
	  octstr_get_cstr(m->group_id));
     
     m->incoming.user = _mms_cfg_getx(cfg, x, octstr_imm("incoming-username"));
     m->incoming.pass = _mms_cfg_getx(cfg, x, octstr_imm("incoming-password"));
     mms_cfg_get_int(cfg, x, octstr_imm("incoming-port"), &m->incoming.port);
#ifdef HAVE_LIBSSL
     mms_cfg_get_bool(cfg, x, octstr_imm("incoming-port-ssl"), &ssl);
#endif
     if ((tmp = mms_cfg_get(cfg, x, octstr_imm("max-throughput"))) != NULL) {   
	  if (octstr_parse_double(&m->throughput, tmp, 0) == -1)
	       m->throughput = 0;
	  mms_info(0,  "mmsbox", NULL,"Set throughput to %.3f for mmsc id <%s>", 
	       m->throughput, octstr_get_cstr(m->id));
	  octstr_destroy(tmp);
     }
     
     type = _mms_cfg_getx(cfg, x, octstr_imm("type"));
     if (octstr_case_compare(type, octstr_imm("eaif")) == 0)
	  m->type = EAIF_MMSC;
     else if (octstr_case_compare(type, octstr_imm("soap")) == 0)
	  m->type = SOAP_MMSC;
     else if (octstr_case_compare(type, octstr_imm("http")) == 0)
	  m->type = HTTP_MMSC;
     else if (octstr_case_compare(type, octstr_imm("custom")) == 0) {
	  m->type = CUSTOM_MMSC;
	  m->settings = _mms_cfg_getx(cfg, x, octstr_imm("custom-settings"));	       
	  /* also load the libary. */
	  if ((m->fns = _mms_load_module(cfg, x, "mmsc-library", "mmsc_funcs", NULL)) == NULL) {
	       mms_error(0,  "mmsbox", NULL, "failed to load MMSC libary functions from module!");
	       m->fns = &dummy_mmsc_funcs;
	  }
     } else 
	  WARNING("MMSBox: Unknown MMSC type [%s]!", 
		  octstr_get_cstr(type));
     if ((xver = _mms_cfg_getx(cfg, x, octstr_imm("mm7-version"))) != NULL && 
	 octstr_len(xver) > 0)
	  sscanf(octstr_get_cstr(xver), 
		 "%d.%d.%d", 
		 &m->ver.major, &m->ver.minor1, &m->ver.minor2);
     else { /* Put in some defaults. */
	  if (m->type == SOAP_MMSC) { 
	       m->ver.major =  MAJOR_VERSION(DEFAULT_MM7_VERSION);
	       m->ver.minor1 = MINOR1_VERSION(DEFAULT_MM7_VERSION);
	       m->ver.minor2 = MINOR2_VERSION(DEFAULT_MM7_VERSION);
	  } else if (m->type == EAIF_MMSC) {
	       m->ver.major  = 3;
	       m->ver.minor1 = 0;
	  }
     }
     
     if ((s = mms_cfg_get(cfg, x, octstr_imm("mm7-soap-xmlns"))) != NULL) {
	  strncpy(m->ver.xmlns, octstr_get_cstr(s), sizeof m->ver.xmlns);	       
	  m->ver.xmlns[-1 + sizeof m->ver.xmlns] = 0; /* NULL terminate, just in case. */
	  octstr_destroy(s);
     } else 
	  m->ver.xmlns[0] = 0;
     
     m->ver.use_mm7_namespace = 1;
     mms_cfg_get_bool(cfg, x, octstr_imm("use-mm7-soap-namespace-prefix"), &m->ver.use_mm7_namespace);
     
     m->default_vasid = mms_cfg_get(cfg, x, octstr_imm("default-vasid"));
     
     octstr_destroy(xver);	 	  
     octstr_destroy(type);
     
     /* Init for filter. */
     if ((s = mms_cfg_get(cfg, x, octstr_imm("mm7-mt-filter-params"))) != NULL) {
	  if (mt_filter)
	       m->use_mt_filter = (mt_filter->init(m->mmsc_url, m->id, s) == 1);
	  else 
	       ERROR("MMSBox: mt-filter-params set for MMSC[%s] but no MT-filter lib "
		     "specified!", 
		     octstr_get_cstr(m->id));
	  if (!m->use_mt_filter)
	       WARNING( "MMSBox: MT MMS filter turned off for MMSC[%s]. Init failed",
		       octstr_get_cstr(m->id));			    
	  octstr_destroy(s);
     } else 
	  m->use_mt_filter = 0;
     
     mms_cfg_get_bool(cfg, x, octstr_imm("reroute"), &m->reroute);
     mms_cfg_get_bool(cfg, x, octstr_imm("reroute-add-sender-to-subject"), &m->reroute_mod_subject);
     m->reroute_mmsc_id = mms_cfg_get(cfg, x, octstr_imm("reroute-mmsc-id"));
     if (m->reroute_mmsc_id != NULL && m->reroute == 0) 
	  WARNING("MMSBox: reroute-mmsc-id parameter set but reroute=false!");
     
     mms_cfg_get_bool(cfg, x, octstr_imm("no-sender-address"), &m->no_senderaddress);
     m->mutex = mutex_create();
     m->incoming.ssl = ssl;
     m->start_time = time(NULL);

     /* finally start the thingie. */
     mmsbox_start_mmsc_conn(m, mmsc_handler_func, errors, warnings);
     if (dict_put_once(mmscs, m->id, m) == 0) {
	  WARNING("Failed to load mmsc [%s]. ID is not unique!", octstr_get_cstr(m->id));
	  mmsbox_stop_mmsc_conn_real(m);
	  free_mmsc_struct(m);
	  m = NULL;
     }

     return m;
}

static void delete_stale_mmsc(int delete_all)
{
     MmscGrp *mmc;
     int n = gwlist_len(mmsc_del_list);
     
     while (n-- > 0 && mmsc_del_list &&
	    (mmc = gwlist_extract_first(mmsc_del_list)) != NULL)
	  if (delete_all ||
	      (mmc->use_count <= 0 && 
	       mmc->delete_after <= time(NULL)))     
	       free_mmsc_struct(mmc);
	  else 
	       gwlist_append(mmsc_del_list, mmc); /* put it back. */

     if (delete_all) {
	  gwlist_destroy(mmsc_del_list, NULL);
	  mmsc_del_list = NULL;
     }
}

int mmsbox_stop_mmsc_conn(Octstr  *mmc_id)
{
     MmscGrp *mmc = dict_remove(mmscs, mmc_id); /* remove it so no one else can get it. */

     if (mmc == NULL) 
	  return -1;

     mmsbox_stop_mmsc_conn_real(mmc);

     mmc->delete_after = time(NULL) + DEFAULT_DELETE_AGE; /* delete after X minutes. */     
     gwlist_append(mmsc_del_list, mmc); /* to be deleted later. */

     delete_stale_mmsc(0); /* Also delete stale ones on each stop. */
     return 0;
}

void mmsbox_stop_all_mmsc_conn(void)
{
     Octstr  *mmc;
     List *l = dict_keys(mmscs);
     
     if (l) {
	  while ((mmc = gwlist_extract_first(l)) != NULL) {
	       mms_info(0,  "mmsbox", NULL,"Stopping MMSC [%s]", octstr_get_cstr(mmc));
	       mmsbox_stop_mmsc_conn(mmc);
	       octstr_destroy(mmc);
	  }	  
	  gwlist_destroy(l, NULL);
     }
}

/* Get the MMC that should handler this recipient. */
MmscGrp *get_handler_mmc(Octstr *id, Octstr *to, Octstr *from)
{
     MmscGrp  *res = NULL;
     int i,  n, not_number;
     Octstr *phonenum = NULL, *xfrom = NULL;
     List *l;
     
     if (id) { /* If ID is set, use it. */
	  res = dict_get(mmscs, id);
	  goto done;
     }

     l = dict_keys(mmscs);
     
     not_number = (octstr_search_char(to, '@', 0) > 0 || 
		   octstr_case_search(to, octstr_imm("/TYPE=IPv"), 0) > 0);	  
     
     /* now try allow/deny stuff. */
     phonenum = extract_phonenum(to, unified_prefix);
     xfrom = extract_phonenum(from, NULL);     
     
     for (i = 0, n = gwlist_len(l); i  < n; i++)  {
	  MmscGrp *mmc = dict_get(mmscs,gwlist_get(l, i));

	  if (mmc == NULL)
	       continue; /* mmsc not there anymore. */
	  
	  if (not_number && 
	      mmc->allowed_prefix == NULL && 
	      mmc->denied_prefix == NULL && 
	      mmc->allowed_sender_prefix == NULL && 
	      mmc->denied_sender_prefix == NULL) 
	       goto loop;
	  else if (not_number) /* don't do tests below if not number. */
	       continue;

	  if (mmc->allowed_prefix && 
	      does_prefix_match(mmc->allowed_prefix, phonenum) == 0)
	       continue; /* does not match. */
	  
	  if (mmc->denied_prefix && 
	      does_prefix_match(mmc->denied_prefix, phonenum) != 0)
	       continue;  /* matches. */

	  if (mmc->allowed_sender_prefix && 
	      does_prefix_match(mmc->allowed_sender_prefix, xfrom) == 0)
	       continue; /* does not match. */
	  
	  if (mmc->denied_sender_prefix && 
	      does_prefix_match(mmc->denied_sender_prefix, xfrom) != 0)
	       continue;  /* matches. */
	  
     loop:
	  res = mmc; /* otherwise it matches, so go away. */
	  break;
     }
     
     octstr_destroy(phonenum);
     octstr_destroy(xfrom);

     gwlist_destroy(l, (void *)octstr_destroy);

done:
     if (res)
	  MMSBOX_MMSC_MARK_INUSE(res); /* Vital! */
     return res;
}



void mmsbox_settings_cleanup(void)
{
     delete_stale_mmsc(1); 

     /* eventually we will destroy the object. For now, we only cleanup queue module. */
     if (qfs) 
	  qfs->mms_cleanup_queue_module();
     
     if (admin_port > 0) {
	  http_close_port(admin_port);
	  if (admin_thread >= 0)
	       gwthread_join(admin_thread);
	  mms_info(0,  "mmsbox", NULL,"Admin port on %d, shutdown", (int)admin_port);
     }
     cdrfs->cleanup();
     mms_event_logger_cleanup();
     mms_cfg_destroy(cfg); /* only delete at end of session. */
     /* More cleanups to follow. */
}

void return_mmsc_conn(MmscGrp *m)
{
     
     if (m)
	  MMSBOX_MMSC_UNMARK_INUSE(m); /* Vital! */     	  

     /* now try and delete as many to-be-deleted mmc as possible */
     delete_stale_mmsc(0);
     
}

/* handle message routing. */
Octstr  *get_mmsbox_queue_dir(Octstr *from, List *to, MmscGrp *m, 
		     Octstr **mmc_id) 
{
    
     if (m && m->reroute) {
	  *mmc_id = m->reroute_mmsc_id ? octstr_duplicate(m->reroute_mmsc_id) : NULL;
	  return outgoing_qdir;
     } else if (m) {
	  Octstr *_mcid, *qdir = NULL;
	  Octstr *fto;
	  
	  if (gwlist_len(to) > 0 && 
	      (fto = gwlist_extract_first(to)) != NULL) { /* we route based on first recipient XXX */
	       Octstr *xto = octstr_duplicate(fto);
	       Octstr *xfrom = octstr_duplicate(from);
	       
	       if (unified_prefix) 
		    _mms_fixup_address(&xfrom, octstr_get_cstr(unified_prefix), strip_prefixes, 0);
	       if (unified_prefix)
		    _mms_fixup_address(&fto, octstr_get_cstr(unified_prefix), strip_prefixes, 0);

	       _mcid = rfs->mmsbox_resolve(xfrom,fto,octstr_get_cstr(m->id), rfs_data, rfs_settings);
	       
	       /* modify what was sent to us. */
	       if (octstr_len(_mcid) == 0) { /* put recipient back, unmodified if incoming only. */	       
		    gwlist_insert(to, 0, xto); 
		    octstr_destroy(fto);
	       } else {
		    if (unified_prefix)
			 _mms_fixup_address(&fto, octstr_get_cstr(unified_prefix), strip_prefixes, 1);
		    
		    gwlist_insert(to, 0, fto);
		    octstr_destroy(xto);
	       }

	       if (xfrom) { /* Check if sender address changed */
		    octstr_delete(from, 0, octstr_len(from));
		    octstr_append(from, xfrom);     
	       }

	       octstr_destroy(xfrom);
	  } else 
	       _mcid = NULL;
	  
	  if (octstr_len(_mcid) == 0) {
	       *mmc_id = NULL;
	       qdir = incoming_qdir;
	  } else {
	       *mmc_id = octstr_duplicate(_mcid);
	       qdir = outgoing_qdir;
	  }

	  octstr_destroy(_mcid);

	  return qdir;
     }
     return 0;
}

static void append_mmsc_status(Octstr *rbody, MmscGrp *m, List *warnings)
{
     time_t t = time(NULL);
     int i, n;
     unsigned long tdiff;
     char lbuf[128], ubuf[128], tmp[64], xport[32];

     char *typ;

     if  (m->type == SOAP_MMSC) 
	  typ = "SOAP";
     else if  (m->type == EAIF_MMSC) 
	  typ = "EAIF";
     else if (m->type == CUSTOM_MMSC)
	  typ = "CUSTOM";
     else 
	  typ = "none";

     if (m->last_pdu > 0) {
	  struct tm tm = gw_localtime(m->last_pdu);
	  gw_strftime(lbuf,sizeof lbuf,  "%x %X", &tm);
     } else 
	  strcpy(lbuf, "n/a");
     
     /* Compute uptime */
     tdiff = t - m->start_time;
     if (tdiff >= 24*3600) {/* we have some days */
	  sprintf(ubuf, "%ld days", tdiff/(24*3600));

	  tdiff %= 24*3600;
     } else 
	  ubuf[0] = 0;
     
     if (tdiff >= 3600) {
	  long x = tdiff/3600;
	  sprintf(tmp, "%s%ld hrs", ubuf[0] ? " " : "", x);
	  strcat(ubuf, tmp);
	  tdiff %= 3600;
     } 

     if (tdiff >= 60) {
	  sprintf(tmp, "%s%ld mins", ubuf[0] ? " " : "", tdiff/60);
	  strcat(ubuf, tmp);
	  tdiff %= 60;
     } 

     if (tdiff > 0) {
	  sprintf(tmp, "%s%ld secs", ubuf[0] ? " " : "", tdiff);
	  strcat(ubuf, tmp);
     } 

     if (m->type != CUSTOM_MMSC)
	  sprintf(xport, "%d", (int)m->incoming.port);
     else 
	  sprintf(xport, "n/a");
     octstr_format_append(rbody, "<mmsc id=\"%S\" type=\"%s\">\n "
			  "<port>%s</port>\n"
			  "<group>%S</group>\n"
			  "<throughput>%.4f</throughput>\n"
			  "<re-route>%s</re-route>\n"
			  "<reroute-mmsc>%S</reroute-mmsc>\n"
			  "<stats>\n"
			  "<uptime>%s</uptime>\n"
			  "<last-pdu>%s</last-pdu>\n"
			  "<mt><pdus>%ld</pdus><errors>%ld</errors></mt>\n"
			  "<mo><pdus>%ld</pdus><errors>%ld</errors></mo>\n"			  
			  "</stats>\n",
			  m->id, 
			  typ,
			  xport,
			  m->group_id ? m->group_id : octstr_imm("n/a"),
			  m->throughput,
			  m->reroute ? "true" : "false",
			  m->reroute_mmsc_id ? m->reroute_mmsc_id : octstr_imm("N/A"),
			  ubuf,
			  lbuf,
			  (long)m->mt_pdus, (long)m->mt_errors,
			  (long)m->mo_pdus, (long)m->mo_errors);


     for (i = 0, n = gwlist_len(warnings); i<n; i++)
	  octstr_format_append(rbody, "<warning>%S</warning>\n", gwlist_get(warnings, i));
     
     octstr_append_cstr(rbody, 	"</mmsc>\n");
}

static void admin_handler(void *unused)
{     
     HTTPClient *client;
     Octstr *ip;
     List   *headers;
     Octstr *url;
     Octstr *body;
     List   *cgivars;     
     Octstr *pass;

     
     mms_info(0,  "mmsbox", NULL,"Admin Interface -- startup on port %d", (int)admin_port);
     
     while((client = http_accept_request(admin_port, 
					 &ip, &url, &headers, 
					 &body, &cgivars)) != NULL) {
	  int flg = -1;	  
	  Octstr *rbody = NULL;
	  int rstatus = HTTP_OK;
	  
	  if (!(flg = is_allowed_ip(admin_allow_ip, admin_deny_ip, ip)) || 
	      ((pass = http_cgi_variable(cgivars, "password")) == NULL  && admin_pass != NULL) ||
	      (admin_pass && octstr_compare(pass, admin_pass) != 0)) {
	       mms_error(0,  "mmsbox", NULL, "Improper access to mmsbox admin interface from IP[%s]: %s",
		     octstr_get_cstr(ip),
		     flg ? "Invalid/empty password" : "IP not allowed");
	       if (flg) {/* means it is allowed by IP */
		    rstatus = HTTP_UNAUTHORIZED;
		    rbody = octstr_imm("<error>Auth failed</error>");
	       }
	  } else {
	       Octstr *mmc_id = http_cgi_variable(cgivars, "mmsc-id");	       
	       List *l = NULL;
	       
	       rbody = octstr_create("<?xml version='1.0'?>\n<mmsbox>\n");
	       /* URI is one of: /status, /start, /stop.
		* mmsc-id is either empty (meaning ALL) or an ID of an existing MMSC connection.
		*/

	       if (octstr_str_case_compare(url, "/start") == 0) {
		    mCfgGrp *m;
		    if (mmc_id == NULL)
			 l = mms_cfg_get_multi(cfg, octstr_imm("mmsc"));
		    else if ((m = mms_get_multi_by_field(cfg, 
							 octstr_imm("mmsc"), 
							 octstr_imm("id"), 
							 mmc_id)) != NULL) {
			 l = gwlist_create();
			 gwlist_append(l, m);
		    }
		    /* Start MMSCs. */
		    if (l)
			 while ((m = gwlist_extract_first(l)) != NULL) {
			      List *e = gwlist_create();
			      List *w = gwlist_create();
			      Octstr *x;
			      MmscGrp *mc = start_mmsc_from_conf(cfg, m, mmsc_receiver_func, e, w);
			      
			      if (mc != NULL)
				   append_mmsc_status(rbody, mc, w);
			      else if (gwlist_len(e) > 0)
				   while ((x = gwlist_extract_first(e)) != NULL) {
					octstr_format_append(rbody, 
							     "<Start-Mmsc><Error>%S</Error></Start-Mmsc>\n",
							     x);				   
					octstr_destroy(x);
				   }
			      gwlist_destroy(e, (void *)octstr_destroy);
			      gwlist_destroy(w, (void *)octstr_destroy);
			      
			      mms_cfg_destroy_grp(cfg, m);			 
			 }		    
	       } else if (octstr_str_case_compare(url, "/stop") == 0) {
		    Octstr *x;
		    if (mmc_id == NULL)
			 l = dict_keys(mmscs);
		    else {
			 l = gwlist_create();
			 gwlist_append(l, octstr_duplicate(mmc_id));
		    }

		    if (l)
			 while ((x = gwlist_extract_first(l)) != NULL) {
			      int ret = mmsbox_stop_mmsc_conn(x);
			      octstr_format_append(rbody, 
						   "<Stop-Mmsc><%s/></Stop-Mmsc>\n",
						   ret == 0 ? "Success" : "Failed");
			      octstr_destroy(x);
			 }
					      
	       } else if (octstr_str_case_compare(url, "/status") == 0) {
		    Octstr *x;
		    MmscGrp *mc;
		    if (mmc_id == NULL)
			 l = dict_keys(mmscs);
		    else {
			 l = gwlist_create();
			 gwlist_append(l, octstr_duplicate(mmc_id));
		    }
		    
		    if (l)
			 while ((x = gwlist_extract_first(l)) != NULL) {
			      if ((mc = dict_get(mmscs, x)) != NULL)
				   append_mmsc_status(rbody, mc, NULL);
			      octstr_destroy(x);
			 }
	       }
	       
	       gwlist_destroy(l, NULL);

	       octstr_append_cstr(rbody, "\n</mmsbox>\n");
	  }
	
	  if (rbody) {
	       List *rh = http_create_empty_headers();
	       http_header_add(rh, "Content-Type", "text/xml");
	       http_send_reply(client, rstatus, rh, rbody);

	       http_destroy_headers(rh);

	  } else 
	       http_close_client(client);

	  octstr_destroy(rbody);	       
	  octstr_destroy(ip);
	  octstr_destroy(url);    
	  octstr_destroy(body);
	  http_destroy_cgiargs(cgivars);
	  http_destroy_headers(headers);
     }

     mms_info(0,  "mmsbox", NULL,"Admin Interface -- shuttind down on port %d", (int)admin_port);
}

void free_mmsbox_http_clientInfo(MmsBoxHTTPClientInfo *h, int freeh)
{     
     octstr_destroy(h->ip);
     octstr_destroy(h->url);    
     octstr_destroy(h->ua); 
     octstr_destroy(h->body);    
     http_destroy_cgiargs(h->cgivars);
     http_destroy_headers(h->headers);

     if (freeh) gw_free(h);
}

static void free_mmsc_struct (MmscGrp *m)
{
     
     gw_assert(m->use_count == 0);

     octstr_destroy(m->id);
     octstr_destroy(m->group_id);
     octstr_destroy(m->vasp_id);
     octstr_destroy(m->mmsc_url);

     octstr_destroy(m->incoming.user);
     octstr_destroy(m->incoming.pass);
     octstr_destroy(m->incoming.allow_ip);
     octstr_destroy(m->incoming.deny_ip);
     octstr_destroy(m->allowed_prefix);
     octstr_destroy(m->denied_prefix);
     octstr_destroy(m->allowed_sender_prefix);
     octstr_destroy(m->denied_sender_prefix);

     octstr_destroy(m->reroute_mmsc_id);

     octstr_destroy(m->settings);
     mutex_destroy(m->mutex);

     gw_free(m);
}

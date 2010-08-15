/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSC CFG: MMC configuration and misc. functions
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
#include "mmsc_cfg.h"
#include "mms_queue.h"


#define MMS_PORT 8191  /* Default content fetch port. */

static void free_vasp(MmsVasp *m);

static void delete_stale_vasps(MmscSettings *settings, int delete_all);
static void admin_handler(MmscSettings *settings);
void mms_cleanup_mmsc_settings(MmscSettings *settings)
{
     /* eventually we will destroy the object. */

     if (settings->admin_port > 0) {
	  http_close_port(settings->admin_port);
	  
	  if (settings->admin_thread >= 0)
	       gwthread_join(settings->admin_thread);
	  
	  mms_info(0,  "mmsc", NULL,"Admin port on %d, shutdown", (int)settings->admin_port);
     }
     mms_cfg_destroy(settings->cfg);
     settings->qfs->mms_cleanup_queue_module();
     if (settings->mm5)
	  settings->mm5->cleanup();
     delete_stale_vasps(settings, 1);
     mms_event_logger_cleanup();
}

MmscSettings *mms_load_mmsc_settings(Octstr *fname, List **proxyrelays, int skip_admin)
{
     mCfg *cfg;
     Octstr *s;
     List *l;
     mCfgGrp *grp;
     mCfgGrp *cgrp;
     MmscSettings *m;
     long port = -1;
     Octstr  *from,  *user, *pass;
     Octstr *qdir = NULL;
     int i, n, xx = 0, ssl = 0;
     void *x;
     

     cfg = mms_cfg_read(fname);
     
     if (cfg == NULL)
	  panic(0, "Couldn't read configuration  from '%s'.", octstr_get_cstr(fname));
     

     cgrp  = mms_cfg_get_single(cfg, octstr_imm("core"));
     grp = mms_cfg_get_single(cfg, octstr_imm("mbuni"));

     m = gw_malloc(sizeof *m);
     memset(m, 0, sizeof *m);

     if (grp == NULL)
	  panic(0,"Missing required group `mbuni' in config file!");

     m->cfg = cfg; /* store it since admin thread needs it. */
     m->vasp_del_list = gwlist_create();

     mms_load_core_settings(cfg, cgrp);

     if ((x = _mms_load_module(cfg, grp, "event-logger-module", "event_logger", NULL)) != NULL) {
	  Octstr *s = mms_cfg_get(cfg, grp, octstr_imm("event-logger-module"));
	  
	  if (mms_event_logger_init(x, s) != 0)
	       panic(0, "Mmsc: Failed to initialise event logger module!");
	  octstr_destroy(s);
     }

     if ((m->mm5 = _mms_load_module(cfg, grp, "mm5-module", "mm5_funcs", shell_mm5)) != NULL) {
	  Octstr *s = mms_cfg_get(cfg, grp, octstr_imm("mm5-module"));
	  
	  if (m->mm5->init(octstr_get_cstr(s)) != 0)
	       panic(0, "Mmsc: Failed to initialise MM5 module!");
	  octstr_destroy(s);
     }

     m->hostname = mms_cfg_get(cfg, grp, octstr_imm("hostname"));

     if (octstr_len(m->hostname) == 0)
	  m->hostname = octstr_create("localhost");
     
     if ((m->host_alias = mms_cfg_get(cfg, grp, octstr_imm("host-alias"))) == NULL)
	  m->host_alias = octstr_duplicate(m->hostname);
     
     if (proxyrelays)
	  *proxyrelays = mms_proxy_relays(cfg, m->hostname);
          
     if (mms_cfg_get_int(cfg, grp, octstr_imm("max-send-threads"), &m->maxthreads) == -1)
	  m->maxthreads = 10;
     
     m->unified_prefix = _mms_cfg_getx(cfg, grp, octstr_imm("unified-prefix"));        
     m->local_prefix = _mms_cfg_getx(cfg, grp, octstr_imm("local-prefixes"));        
     
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("strip-prefixes"))) != NULL) {
	  m->strip_prefixes = octstr_split(s, octstr_imm(";"));
	  octstr_destroy(s);
     } else 
	  m->strip_prefixes = NULL;
     
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("mmsc-services"))) != NULL) {
	  char *p = octstr_get_cstr(s), *q, *r;
	  m->svc_list = 0;
	  
	  for (q = strtok_r(p, ",; ", &r); 
	       q; 
	       q = strtok_r(NULL, ",; ", &r)) 
	       if (strcasecmp(q, "MM1") == 0)
		    m->svc_list |= SvcMM1;
	       else if (strcasecmp(q, "MM7") == 0)
		    m->svc_list |= SvcMM7;
	       else if (strcasecmp(q, "Relay") == 0)
		    m->svc_list |= SvcRelay;
	       else if (strcasecmp(q, "All") == 0)
		    m->svc_list = (SvcRelay | SvcMM1 | SvcMM7);	       
	       else 
		    mms_warning(0,  "mmsc", NULL,"unknown mmsc-service [%s]. Skipped.", q);		    
	  octstr_destroy(s);
     } else 
	  m->svc_list = ~0U;

     m->name = _mms_cfg_getx(cfg, grp, octstr_imm("name"));

     m->sendmail = _mms_cfg_getx(cfg, grp, octstr_imm("send-mail-prog"));      
     
     qdir = _mms_cfg_getx(cfg, grp, octstr_imm("storage-directory"));     
     if (qdir && octstr_len(qdir) >= QFNAMEMAX)
	  mms_warning(0,  "mmsc", NULL,"storage-directory name too long. Max length is %d", QFNAMEMAX);

     if (mkdir(octstr_get_cstr(qdir), 
	       S_IRWXU|S_IRWXG) < 0 && 
	 errno != EEXIST)
	  panic(0, "Failed to create queue directory: %s - %s!",
		octstr_get_cstr(qdir), strerror(errno));

     if ((m->qfs = _mms_load_module(cfg, grp, "queue-manager-module", "qfuncs", NULL)) == NULL) {
	  m->qfs = &default_qfuncs; /* default queue handler. */
	  m->qfs->mms_init_queue_module(qdir, 
					octstr_get_cstr(qdir), 
					(2 + 2 + 2)*m->maxthreads); /* We expect 2 max for each mmsrelay component (= 4)
									   * + 2 for mmsproxy (on for mm1proxy, one for mm7proxy
									   */
     } else {
	  Octstr *s = _mms_cfg_getx(cfg, grp, octstr_imm("queue-module-init-data"));
	  if (m->qfs->mms_init_queue_module(s, octstr_get_cstr(qdir), 
					    (2 + 2 + 2)*m->maxthreads) != 0)
	       panic(0, "failed to initialise queue module, with data: %s",
		     octstr_get_cstr(s));
	  octstr_destroy(s);
     }
     
     if ((m->global_queuedir = m->qfs->mms_init_queue_dir("global", &xx)) == NULL ||
	 xx != 0)
	  panic(0, "Failed to initialise global queue directory: %s - %s!", 
		octstr_get_cstr(m->global_queuedir), strerror(errno));

     if ((m->mm1_queuedir = m->qfs->mms_init_queue_dir("mm1", &xx)) == NULL ||
	 xx != 0)
	  panic(0, "Failed to initialise local queue directory: %s - %s!", 
		octstr_get_cstr(m->mm1_queuedir), strerror(errno));

     m->mmbox_rootdir = octstr_format("%S/mmbox", qdir);     
     if (mmbox_root_init(octstr_get_cstr(m->mmbox_rootdir)) != 0)
	  panic(0, "Failed to initialise mmbox root directory, error: %s!", 
		strerror(errno));
     
     m->ua_profile_cache_dir = octstr_format("%S/UserAgent_Profiles", qdir);
     
     if (mkdir(octstr_get_cstr(m->ua_profile_cache_dir), 
	       S_IRWXU|S_IRWXG) < 0 && 
	 errno != EEXIST)
	  panic(0, "Failed to initialise UA Profile directory, error: %s!",
		strerror(errno));

     if (mms_cfg_get_int(cfg, grp, octstr_imm("maximum-send-attempts"), &m->maxsendattempts) == -1)
	  m->maxsendattempts = MAXQTRIES;

     if (mms_cfg_get_int(cfg, grp, octstr_imm("mm1-maximum-notify-attempts"), &m->mm1_maxsendattempts) == -1)
	  m->mm1_maxsendattempts = m->maxsendattempts;

     if (mms_cfg_get_int(cfg, grp, octstr_imm("default-message-expiry"), &m->default_msgexpiry) == -1)
	  m->default_msgexpiry = DEFAULT_EXPIRE;

     if (mms_cfg_get_int(cfg, grp, octstr_imm("max-message-expiry"), &m->max_msgexpiry) == -1)
	  m->max_msgexpiry = -1;
     
     s = _mms_cfg_getx(cfg, grp, octstr_imm("queue-run-interval"));
     if (!s || (m->queue_interval = atof(octstr_get_cstr(s))) <= 0)
	  m->queue_interval = QUEUERUN_INTERVAL;


     octstr_destroy(s);


     s = _mms_cfg_getx(cfg, grp, octstr_imm("mm1-queue-run-interval"));
     if (!s || (m->mm1_queue_interval = atof(octstr_get_cstr(s))) <= 0)
	  m->mm1_queue_interval = m->queue_interval;

     octstr_destroy(s);
     
     if (mms_cfg_get_int(cfg, grp, octstr_imm("send-attempt-back-off"), &m->send_back_off) == -1)
	  m->send_back_off = BACKOFF_FACTOR;

     /* Make send sms url. */
     m->sendsms_url = _mms_cfg_getx(cfg, grp, octstr_imm("sendsms-url"));     
     
     user = mms_cfg_get(cfg, grp, octstr_imm("sendsms-username"));     
     pass = mms_cfg_get(cfg, grp, octstr_imm("sendsms-password"));     
     from = mms_cfg_get(cfg, grp, octstr_imm("sendsms-global-sender"));     
     
     i = octstr_search_char(m->sendsms_url, '?', 0); /* If ? is in there, omit below. */

     octstr_format_append(m->sendsms_url, 
			  (from && octstr_len(from) > 1) ? 
			  "%sfrom=%E" : "%s_dummy=x",	  
			  (i >= 0) ? "" : "?", from);	
   
     if (user && octstr_len(user) > 0)
	  octstr_format_append(m->sendsms_url,
			       "&username=%E&password=%E",
			       user, pass);
     m->system_user = octstr_format("system-user@%S", 
				    m->hostname);	  
     octstr_destroy(user);
     octstr_destroy(pass);
     octstr_destroy(from);
     
     mms_cfg_get_int(cfg, grp, octstr_imm("mms-port"), &port);

     m->port = (port > 0) ? port : MMS_PORT;

     m->mm7port = -1;
     mms_cfg_get_int(cfg, grp, octstr_imm("mm7-port"), &m->mm7port);

     m->allow_ip = _mms_cfg_getx(cfg, grp, octstr_imm("allow-ip"));          
     m->deny_ip = _mms_cfg_getx(cfg, grp, octstr_imm("deny-ip"));          

     m->email2mmsrelay_hosts = _mms_cfg_getx(cfg, grp, 
					   octstr_imm("email2mms-relay-hosts"));
     
     m->prov_notify = _mms_cfg_getx(cfg, grp,octstr_imm("prov-server-notify-script"));

     m->prov_getstatus = _mms_cfg_getx(cfg, grp,octstr_imm("prov-server-sub-status-script"));     
     m->mms_notify_txt = _mms_cfg_getx(cfg, grp, octstr_imm("mms-notify-text"));
     m->mms_notify_unprov_txt = _mms_cfg_getx(cfg, grp, octstr_imm("mms-notify-unprovisioned-text"));


     m->mms_email_txt = _mms_cfg_getx(cfg, grp, octstr_imm("mms-to-email-txt"));
     m->mms_email_html = _mms_cfg_getx(cfg, grp, octstr_imm("mms-to-email-html"));
     m->mms_email_subject = mms_cfg_get(cfg, grp, octstr_imm("mms-to-email-default-subject"));

     m->mms_toolarge = _mms_cfg_getx(cfg, grp, octstr_imm("mms-message-too-large-txt"));

     
     if ((s =  mms_cfg_get(cfg, grp, octstr_imm("mms-client-msisdn-header"))) == NULL)
	  s = octstr_imm(XMSISDN_HEADER);     
     m->wap_gw_msisdn_header = octstr_split(s, octstr_imm(","));
     octstr_destroy(s);

     if ((s = mms_cfg_get(cfg, grp, octstr_imm("mms-client-ip-header"))) == NULL)
	  s =  octstr_imm(XIP_HEADER);
     m->wap_gw_ip_header = octstr_split(s, octstr_imm(","));
     octstr_destroy(s);

     mms_cfg_get_bool(cfg, grp, octstr_imm("notify-unprovisioned"), &m->notify_unprovisioned);

     m->billing_params = _mms_cfg_getx(cfg, grp, 
				  octstr_imm("billing-module-parameters"));
     /* Get and load the billing lib if any. */
     
     if ((m->mms_billfuncs = _mms_load_module(cfg, grp, "billing-library",  "mms_billfuncs", 
	       &mms_billfuncs_shell)) != NULL) {
	  if (m->mms_billfuncs->mms_billingmodule_init == NULL ||
	      m->mms_billfuncs->mms_billmsg == NULL ||
	      m->mms_billfuncs->mms_billingmodule_fini == NULL ||
	      m->mms_billfuncs->mms_logcdr == NULL) 
	       panic(0, "Missing or NULL functions in billing module!");
     } else 
	  m->mms_billfuncs = &mms_billfuncs; /* The default one. */	  
     
     m->mms_bill_module_data = m->mms_billfuncs->mms_billingmodule_init(octstr_get_cstr(m->billing_params));
     
     m->resolver_params = _mms_cfg_getx(cfg, grp, 
				   octstr_imm("resolver-module-parameters"));
     
     /* Get and load the resolver lib if any. */
     if ((m->mms_resolvefuncs = _mms_load_module(cfg, grp, "resolver-library",  
					    "mms_resolvefuncs", 
	       &mms_resolvefuncs_shell)) != NULL) {
	  if (m->mms_resolvefuncs->mms_resolvermodule_init == NULL ||
	      m->mms_resolvefuncs->mms_resolve == NULL ||
	      m->mms_resolvefuncs->mms_resolvermodule_fini == NULL)
	       panic(0, "Missing or NULL functions in resolver module!");
     } else 
	  m->mms_resolvefuncs = &mms_resolvefuncs;	/* The default one. */

     m->mms_resolver_module_data = m->mms_resolvefuncs->mms_resolvermodule_init(octstr_get_cstr(m->resolver_params));

     m->detokenizer_params = _mms_cfg_getx(cfg, grp, octstr_imm("detokenizer-module-parameters"));
     
     /* Get and load the detokenizer lib if any. */
     if ((m->mms_detokenizefuncs = _mms_load_module(cfg, grp, "detokenizer-library", 
					       "mms_detokenizefuncs",
	       &mms_detokenizefuncs_shell)) != NULL) {
	  if (m->mms_detokenizefuncs->mms_detokenizer_init == NULL ||
	      m->mms_detokenizefuncs->mms_detokenize == NULL ||
	      m->mms_detokenizefuncs->mms_gettoken == NULL ||
	      m->mms_detokenizefuncs->mms_detokenizer_fini == NULL)
	       panic(0, "Missing or NULL functions in detokenizer module!");
	  if (m->mms_detokenizefuncs->mms_detokenizer_init(octstr_get_cstr(m->detokenizer_params)) != 0)
	      panic(0, "Detokenizer module failed to initialize");
     } else 
	  m->mms_detokenizefuncs = NULL;

     if (mms_cfg_get_bool(cfg, grp, octstr_imm("allow-ip-type"), &m->allow_ip_type)  < 0)
	  m->allow_ip_type = 1;
     
     mms_cfg_get_bool(cfg, grp, octstr_imm("optimize-notification-size"), 
		      &m->optimize_notification_size);

     if (mms_cfg_get_bool(cfg, grp, octstr_imm("content-adaptation"), &m->content_adaptation) < 0)
	  m->content_adaptation = 1;

     if (mms_cfg_get_bool(cfg, grp, octstr_imm("send-dlr-on-fetch"), &m->dlr_on_fetch) < 0)
	  m->dlr_on_fetch = 0;
     

     octstr_destroy(qdir);
     
     /* Now load the VASP list. */
     l = mms_cfg_get_multi(cfg, octstr_imm("mms-vasp"));
     m->vasp_list = dict_create(53, NULL); 
     for (i=0, n=gwlist_len(l); i<n; i++)  {
	  List *e = gwlist_create();
	  List *w = gwlist_create();
	  Octstr *x;
	  mCfgGrp *xgrp = gwlist_get(l, i);
	  mmsc_load_vasp_from_conf(m, xgrp, e, w);

	  while ((x = gwlist_extract_first(e)) != NULL) {
	       mms_error(0,  "mmsc", NULL, "%s", octstr_get_cstr(x));
	       octstr_destroy(x);
	  }

	  while ((x = gwlist_extract_first(w)) != NULL) {
	       mms_warning(0,  "mmsc", NULL, "%s", octstr_get_cstr(x));
	       octstr_destroy(x);
	  }
	  gwlist_destroy(e, NULL);
	  gwlist_destroy(w, NULL);

	  mms_cfg_destroy_grp(cfg, xgrp);
     }
     gwlist_destroy(l, NULL);

     /* Now load & start admin interface. */

     if (skip_admin)
	  m->admin_port = -1;
     else {
	  mms_cfg_get_int(cfg, grp, octstr_imm("mmsc-admin-port"), &m->admin_port);
#ifdef HAVE_LIBSSL
	  mms_cfg_get_bool(cfg, grp, octstr_imm("admin-port-ssl"), &ssl);
#endif
	  m->admin_pass = mms_cfg_get(cfg, grp, octstr_imm("admin-password"));
	  
	  m->admin_allow_ip = mms_cfg_get(cfg, grp, octstr_imm("admin-allow-ip"));
	  m->admin_deny_ip = mms_cfg_get(cfg, grp, octstr_imm("admin-deny-ip"));
     
	  if (m->admin_port > 0 &&
	      http_open_port(m->admin_port, ssl)< 0) {
	       mms_error(0,  "mmsc", NULL, "Failed to start admin server on port %d: %s",
			 (int)m->admin_port, strerror(errno));
	       m->admin_port = -1;
	  } else if (m->admin_port > 0 &&
		     (m->admin_thread = gwthread_create((gwthread_func_t *)admin_handler, m)) < 0) {
	       mms_error(0,  "mmsc", NULL, "Failed to start admin server thread: %s",
			 strerror(errno));
	       http_close_port(m->admin_port);
	       m->admin_port = -1;
	  } else if (m->admin_pass == NULL) 
	       mms_warning(0,  "mmsc", NULL, "Empty or no password supplied for admin port. All requests will be allowed!");
	  
     }
     mms_cfg_destroy_grp(cfg, cgrp);
     mms_cfg_destroy_grp(cfg, grp);

     return m;
}

int mmsc_unload_vasp(MmscSettings *m, Octstr *id)
{

     MmsVasp *mv = dict_remove(m->vasp_list, id);

     if (mv == NULL)
	  return -1;

     if (m->mms2email == mv)
	  m->mms2email = NULL;

     if (m->mms2mobile == mv)
	  m->mms2mobile = NULL;
     
     mv->delete_after = time(NULL) + DEFAULT_DELETE_AGE;
     gwlist_append(m->vasp_del_list, mv);

     delete_stale_vasps(m, 0);
     return 0;
}

#define ERROR(fmt,...) do {if (errors) gwlist_append(errors, octstr_format((fmt), ##__VA_ARGS__));} while(0)
#define WARNING(fmt,...) do {if (warnings) gwlist_append(warnings, octstr_format((fmt), ##__VA_ARGS__));}while(0)

MmsVasp *mmsc_load_vasp_from_conf(MmscSettings *m, mCfgGrp *grp, List *errors, List *warnings)
{
     mCfg *cfg = m->cfg;
     MmsVasp *mv = gw_malloc(sizeof *mv), *ret = NULL;
     Octstr *s;
     int ibool = 0;
     long short_code;
     
     memset(mv, 0, sizeof mv[0]);
     
     mv->throughput = -1;
     mv->num_short_codes = 0;
     
     mv->id = _mms_cfg_getx(cfg, grp, octstr_imm("vasp-id"));	  
     
     if (mms_cfg_get_int(cfg, grp, octstr_imm("short-code"), &short_code) == 0) {
	  WARNING("'short-code' is deprecated, please use 'short-codes'"); 
	  mv->num_short_codes = 1;
	  mv->short_codes = gw_malloc(sizeof mv->short_codes[0]);
	  mv->short_codes[0] = short_code;
     } else if ((s = mms_cfg_get(cfg, grp, octstr_imm("short-codes"))) != NULL)  {
	  List *l = octstr_split(s, octstr_imm(";"));
	  Octstr *x;
	  mv->short_codes = gw_malloc((gwlist_len(l) + 1) * sizeof mv->short_codes[0]);
	  
	  while ((x = gwlist_extract_first(l)) != NULL) {
	       if ((short_code = strtoul(octstr_get_cstr(x), NULL, 10)) == 0 && 
		   (errno == EINVAL || errno == ERANGE)) 
		    ERROR("Invalid short-code format: %S", x);
	       else 
		    mv->short_codes[mv->num_short_codes++] = short_code;
	       octstr_destroy(x);
	  }
	  gwlist_destroy(l, NULL);
	  octstr_destroy(s);
     }

     if (mv->num_short_codes == 0) {
	  ERROR("No valid short codes defined, VASP cannot be loaded");
	  free_vasp(mv);
	  goto done;
     }
     
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("throughput"))) != NULL) {	       
	  mv->throughput = atof(octstr_get_cstr(s));
	  octstr_destroy(s);
     }
     
     mv->vasp_username = _mms_cfg_getx(cfg, grp, octstr_imm("vasp-username"));
     mv->vasp_password = _mms_cfg_getx(cfg, grp, octstr_imm("vasp-password"));
     
     mv->vasp_url = _mms_cfg_getx(cfg, grp, octstr_imm("vasp-url"));
     
     s = _mms_cfg_getx(cfg, grp, octstr_imm("type"));
     
     if (octstr_case_compare(s, octstr_imm("soap")) == 0)
	  mv->type = SOAP_VASP;
     else if (octstr_case_compare(s, octstr_imm("eaif")) == 0)
	  mv->type = EAIF_VASP;
     else
	  mv->type = NONE_VASP;
     octstr_destroy(s);
     
     mv->ver.major = mv->ver.minor1 = mv->ver.minor2 = 0;
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("mm7-version"))) != NULL && 
	 octstr_len(s) > 0) 
	  sscanf(octstr_get_cstr(s), "%d.%d.%d", &mv->ver.major, &mv->ver.minor1, &mv->ver.minor2);
     else {
	  if (mv->type == SOAP_VASP) {
	       mv->ver.major =  MAJOR_VERSION(DEFAULT_MM7_VERSION);
	       mv->ver.minor1 = MINOR1_VERSION(DEFAULT_MM7_VERSION);
	       mv->ver.minor2 = MINOR2_VERSION(DEFAULT_MM7_VERSION);
	  } else if (mv->type == EAIF_VASP) {
	       mv->ver.major  = 3;
	       mv->ver.minor1 = 0;
	  }	       
     }
     octstr_destroy(s);
     
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("mm7-soap-xmlns"))) != NULL) {
	  strncpy(mv->ver.xmlns, octstr_get_cstr(s), sizeof mv->ver.xmlns);	       
	  
	  mv->ver.xmlns[-1 + sizeof mv->ver.xmlns] = 0; /* NULL terminate, just in case. */
	  octstr_destroy(s);
     } else 
	  mv->ver.xmlns[0] = 0;
     
     mv->ver.use_mm7_namespace = 1;
     mms_cfg_get_bool(cfg, grp, octstr_imm("use-mm7-soap-namespace-prefix"), &mv->ver.use_mm7_namespace);
     
     /* Set the handler vasp accounts. */
     if (mms_cfg_get_bool(cfg, grp, octstr_imm("mms-to-email-handler"), &ibool) == 0 &&
	 ibool) {
	  if (m->mms2email)
	      WARNING("mms-to-email handler VASP res-et!.");
	  m->mms2email = mv;
     }	  
     if (mms_cfg_get_bool(cfg, grp, octstr_imm("mms-to-local-copy-handler"), &ibool) == 0 &&
	 ibool) {
	  if (m->mms2mobile)
	       WARNING("mms-to-mobile copy handler VASP re-set."); 
	  m->mms2mobile = mv;
     }
     
     if ((s = mms_cfg_get(cfg, grp, octstr_imm("send-uaprof"))) != NULL){ 	       
	  if (octstr_str_case_compare(s, "url") == 0)
	       mv->send_uaprof = UAProf_URL;
	  else if (octstr_str_case_compare(s, "ua") == 0)
	       mv->send_uaprof = UAProf_UA;
	  else {
	       WARNING("unknown send-uaprof value '%s'. Must be \"ua\" or \"url\"!", 
			   octstr_get_cstr(s)); 
	       mv->send_uaprof = UAProf_None;
	  }
	  octstr_destroy(s);
     }
     mv->stats.start_time = time(NULL);

     if (dict_put_once(m->vasp_list, mv->id, mv) == 0) {
	  ERROR("Failed to load vasp [%S]. ID is not unique!", mv->id);
	  free_vasp(mv);
     } else 
	  ret = mv;
     
done:
     return ret;
}

List *mms_proxy_relays(mCfg *cfg, Octstr *myhostname)
{
     List *gl = mms_cfg_get_multi(cfg, octstr_imm("mmsproxy"));
     int i, n;
     List *l = gwlist_create();
     
     for (i = 0, n = gwlist_len(gl); i < n; i++) {
	  mCfgGrp *grp = gwlist_get(gl, i);
	  MmsProxyRelay *m = gw_malloc(sizeof *m);
	  Octstr *s;

	  m->host = _mms_cfg_getx(cfg, grp, octstr_imm("host"));
	  m->name = _mms_cfg_getx(cfg, grp, octstr_imm("name"));
	  m->allowed_prefix = _mms_cfg_getx(cfg, grp, octstr_imm("allowed-prefix"));
	  m->denied_prefix = _mms_cfg_getx(cfg, grp, octstr_imm("denied-prefix"));	  
	  if (mms_cfg_get_bool(cfg, grp, octstr_imm("confirmed-delivery"), &m->confirmed_mm4) < 0)
	       m->confirmed_mm4 = 1;
	  
	  m->sendmail = mms_cfg_get(cfg, grp, octstr_imm("send-mail-prog"));      	  
	  m->unified_prefix = mms_cfg_get(cfg, grp, octstr_imm("unified-prefix"));          
	  if ((s = mms_cfg_get(cfg, grp, octstr_imm("strip-prefixes"))) != NULL) {
	       m->strip_prefixes = octstr_split(s, octstr_imm(";"));
	       octstr_destroy(s);
	  } else 
	       m->strip_prefixes = NULL;

	  if (octstr_compare(m->host, myhostname) == 0)
	       mms_warning(0,  "mmsc", NULL,"MMSC Config: Found MM4 Proxy %s with same hostname as local host!", 
		       octstr_get_cstr(m->name));
	  gwlist_append(l, m);

	  mms_cfg_destroy_grp(cfg, grp);
     }
     
     gwlist_destroy(gl, NULL);

     return l;
}

Octstr *mms_makefetchurl(char *qf, Octstr *token, int loc,
			 Octstr *to,
			 MmscSettings *settings)
{
     Octstr *url = octstr_create("");
     Octstr *host_alias = settings->host_alias;
     Octstr *hstr; 
     Octstr *endtoken, *x; 

     MmsDetokenizerFuncStruct *tfs = settings->mms_detokenizefuncs;
     
     if (host_alias && octstr_len(host_alias) > 0 &&
	 octstr_compare(host_alias, settings->hostname) != 0)
	  hstr = octstr_duplicate(host_alias);
     else 
	  hstr = octstr_format("%S:%d",
			       settings->hostname, settings->port);
     
     octstr_format_append(url, "http://%S/%s@%d", 
			  hstr, 
			  qf, loc);
     
     if (tfs && tfs->mms_gettoken) { /* we append the recipient token or we append the message token. */
	  endtoken =  tfs->mms_gettoken(to); 
	  if (!endtoken) 
	       endtoken = octstr_imm("x");
     } else {
	  if (!token) 
	       endtoken = octstr_imm("x");	  
	  else
	       endtoken = octstr_duplicate(token);
     }
     
     x = octstr_duplicate(endtoken); /* might be immutable, so we duplicate it. */
     octstr_url_encode(x);
     octstr_format_append(url, "/%S", x);

     octstr_destroy(endtoken);
     octstr_destroy(x);
     octstr_destroy(hstr);
     return url;
}

static Octstr *xfind_one_header(List *req_hdrs, List *hdr_names)
{

     int i;
     
     for (i = 0; i<gwlist_len(hdr_names); i++) {
	  Octstr *s = gwlist_get(hdr_names, i);
	  Octstr *x = s ? http_header_value(req_hdrs, s) : NULL;	  

	  if (x) 
	       return x;
     }
     return NULL;
}

Octstr *mms_find_sender_msisdn(Octstr *send_url, 
			       Octstr *ip,
			       List *request_hdrs, 
			       List *msisdn_header, 
			       List *requestip_header,
			       MmsDetokenizerFuncStruct* detokenizerfuncs)
{
     /* Either we have a WAP gateway header as defined, or we look for 
      * last part of url, pass it to detokenizer lib if defined, and back comes our number.
      */
     
     Octstr *phonenum = xfind_one_header(request_hdrs, 
					 msisdn_header);
     
     if (phonenum == NULL || octstr_len(phonenum) == 0) {
	  List *l  = octstr_split(send_url, octstr_imm("/"));
	  int len = gwlist_len(l);
	  Octstr *xip = xfind_one_header(request_hdrs, 
					 requestip_header);
	  if (xip == NULL)
	       xip = ip ? octstr_duplicate(ip) : NULL;
	  if (detokenizerfuncs && (len > 1 || xip)) 
	       phonenum = detokenizerfuncs->mms_detokenize(len > 1 ?  gwlist_get(l, len - 1) : send_url, 
							   xip);	  

	  gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
	  octstr_destroy(xip);
     }
     
     return phonenum;     
}

Octstr *mms_find_sender_ip(List *request_hdrs, List *ip_header, Octstr *ip, int *isv6)
{
     Octstr *xip;
     /* Look in the headers, if none is defined, return actual IP */
     Octstr *client_ip = xfind_one_header(request_hdrs, ip_header);
     char *s;
     
     xip = client_ip  ? client_ip : octstr_duplicate(ip);
     
     s = octstr_get_cstr(xip);

     /* Crude test for ipv6 */
     *isv6 = (index(s, ':') != NULL);
     return xip;
}

int mms_decodefetchurl(Octstr *fetch_url,  
		       Octstr **qf, Octstr **token, int *loc)
{
     Octstr *xfurl = octstr_duplicate(fetch_url);
     int i, j, n;
     char *s, *p;
     
     for (i = 0, n = 0, s = octstr_get_cstr(xfurl); 
	  i < octstr_len(xfurl); i++)
	  if (s[i] == '/')
	       n++;
     if (n < 2) /* We need at least two slashes. */
	  octstr_append_char(xfurl, '/');
     
     i = 0;
     n = octstr_len(xfurl);
     s = octstr_get_cstr(xfurl);
     
     p = strrchr(s, '/'); /* Find last slash. */
     if (p)
	  i = (p - s) - 1;
     else
	  i = n-1;
     
     if (i < 0)
	  i = 0;
     
     while (i>0 && s[i] != '/')
	  i--; /* Go back, find first slash */
     if (i>=0 && s[i] == '/')
	  i++;
     
     /* Now we have qf, find its end. */
     
     j = i;     
     while (j<n && s[j] != '/')
	  j++; /* Skip to next slash. */
     
     *qf = octstr_copy(fetch_url, i, j-i);
     
     if (j<n)
	  *token = octstr_copy(fetch_url, j + 1, n - (j+1));
     else
	  *token = octstr_create("");
     octstr_destroy(xfurl);

     /* Now get loc out of qf. */
     *loc = MMS_LOC_MQUEUE;
     i = octstr_search_char(*qf, '@', 0);
     if (i >= 0) {
	  long l;
	  int j = octstr_parse_long(&l, *qf, i+1, 10);
	  if (j > 0)
	       *loc = l;
	  octstr_delete(*qf, i, octstr_len(*qf));
     } 
     
     return 0;
}


int mms_ind_send(Octstr *prov_cmd, Octstr *to)
{
     Octstr *tmp;
     Octstr *s;
     int res = 1;
     
     if (prov_cmd == NULL ||
	 octstr_len(prov_cmd) == 0)
	  return 1;
     
     tmp = octstr_duplicate(to);
     escape_shell_chars(tmp);     
     s = octstr_format("%S %S", prov_cmd, tmp); 
     octstr_destroy(tmp);

     if (s) {
	  int x = system(octstr_get_cstr(s));
	  int y = WEXITSTATUS(x);

	  if (x < 0) {
	       mms_error(0, "mmsc", NULL, "Checking MMS Ind.Send: Failed to run command %s!", 
		     octstr_get_cstr(s));
	       res = 1;	
	  } else if (y != 0 && y != 1)
	       res =  -1;
	  else 
	       res = y;
	  octstr_destroy(s);
     } else 
	  mms_warning(0,  "mmsc", NULL, "Checking MMS Ind.Send: Failed call to compose command [%s] ", 
	       octstr_get_cstr(prov_cmd));

	  
     return res;
}

void notify_prov_server(char *cmd, char *from, char *event, char *arg, Octstr *msgid, 
			Octstr *ua, Octstr *uaprof)
{
     Octstr *s;
     Octstr *tmp, *tmp2, *tmp3, *tmp4;
     
     if (cmd == NULL || cmd[0] == '\0')
	  return;

     gw_assert(from);
     gw_assert(cmd);
     gw_assert(event);

     tmp = octstr_create(from);     
     tmp2 = msgid ? octstr_duplicate(msgid) : octstr_create("");     
     tmp3 = ua ? octstr_duplicate(ua) : octstr_create("");
     tmp4 = uaprof ? octstr_duplicate(uaprof) : octstr_create("");

     escape_shell_chars(tmp);     
     escape_shell_chars(tmp2);
     escape_shell_chars(tmp3);
     escape_shell_chars(tmp4);
     
     s = octstr_format("%s '%s' '%S' '%s' '%S' '%S' '%S'", cmd, event, 
		       tmp, arg, tmp2, tmp3, tmp4); 

     system(octstr_get_cstr(s));	  
     
     octstr_destroy(s);
     octstr_destroy(tmp);
     octstr_destroy(tmp2);
     octstr_destroy(tmp3);
     octstr_destroy(tmp4);     
}

static void free_vasp(MmsVasp *m)
{
     if (m == NULL) 
	  return;
     octstr_destroy(m->id);
     octstr_destroy(m->vasp_username);
     octstr_destroy(m->vasp_password);
     octstr_destroy(m->vasp_url);
     
     if (m->short_codes)
	  gw_free(m->short_codes);
     
     gw_free(m);
}

static void delete_stale_vasps(MmscSettings *settings, int delete_all)
{
     MmsVasp *mv;
     int n = gwlist_len(settings->vasp_del_list);
     
     while (n-- > 0 && 
	    (mv = gwlist_extract_first(settings->vasp_del_list)) != NULL)
	  if (delete_all ||
	      mv->delete_after <= time(NULL))     
	       free_vasp(mv);
	  else 
	       gwlist_append(settings->vasp_del_list, mv); /* put it back. */
     
     if (delete_all) {
	  gwlist_destroy(settings->vasp_del_list, NULL);
	  settings->vasp_del_list = NULL;
     }
}


static void append_vasp_status(Octstr *rbody, MmsVasp *m, List *warnings)
{
     time_t t = time(NULL);
     int i, n;
     unsigned long tdiff;
     char lbuf[128], ubuf[128], tmp[64], mm7ver[64] = "n/a";

     char *typ;

     
     if  (m->type == SOAP_VASP) {
	  typ = "SOAP";
	  sprintf(mm7ver, "%d.%d.%d", m->ver.major, m->ver.minor1, m->ver.minor2);	  
     }  else if  (m->type == EAIF_VASP) {
	  typ = "EAIF";
	  sprintf(mm7ver, EAIF_VERSION, m->ver.major, m->ver.minor1);
     } else 
	  typ = "none";

     if (m->stats.last_pdu > 0) {
	  struct tm tm = gw_localtime(m->stats.last_pdu);
	  gw_strftime(lbuf,sizeof lbuf,  "%x %X", &tm);
     } else 
	  strcpy(lbuf, "n/a");
     
     /* Compute uptime */
     tdiff = t - m->stats.start_time;
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

     if (m->throughput > 0)
	  sprintf(tmp, "%.2f", m->throughput);
     else 
	  sprintf(tmp, "n/a");
     octstr_format_append(rbody, "<vasp id=\"%S\" type=\"%s\">\n "
			  "<vasp-url>%S</vasp-url>\n"
			  "<incoming-username>%S</incoming-username>\n"
			  "<throughput>%s</throughput>\n"
			  "<mm7-version>%s</mm7-version>\n"
			  "<send-ua-prof>%s</send-ua-prof>\n"
			  "<stats>\n"
			  "<uptime>%s</uptime>\n"
			  "<last-pdu>%s</last-pdu>\n"
			  "<outgoing><pdus>%ld</pdus><errors>%ld</errors></outgoing>\n"
			  "<incoming><pdus>%ld</pdus><errors>%ld</errors></incoming>\n"			  
			  "</stats>\n"
			  "<short-codes>\n" ,
			  m->id, 
			  typ,
			  m->vasp_url,
			  m->vasp_username,
			  tmp,
			  mm7ver,
			  m->send_uaprof == UAProf_URL ? 
			  "url" : (m->send_uaprof == UAProf_UA ? "user-agent" : "n/a"),			 
			  ubuf,
			  lbuf,
			  (long)m->stats.mt_pdus, (long)m->stats.mt_errors,
			  (long)m->stats.mo_pdus, (long)m->stats.mo_errors);
     

     for (i = 0; i<m->num_short_codes; i++)
	  octstr_format_append(rbody, "<short-code>%ld</short-code>\n", m->short_codes[i]);
     
     octstr_append_cstr(rbody, "</short-codes>\n");

     for (i = 0, n = gwlist_len(warnings); i<n; i++)
	  octstr_format_append(rbody, "<warning>%S</warning>\n", gwlist_get(warnings, i));
     
     octstr_append_cstr(rbody, 	"</vasp>\n");
}

static void admin_handler(MmscSettings *settings)
{

     HTTPClient *client;
     Octstr *ip;
     List   *headers;
     Octstr *url;
     Octstr *body;
     List   *cgivars;     
     Octstr *pass;
     
     mms_info(0,  "mmsc", NULL,"Admin Interface -- startup on port %d", (int)settings->admin_port);
     
     while((client = http_accept_request(settings->admin_port, 
					 &ip, &url, &headers, 
					 &body, &cgivars)) != NULL) {
	  int flg = -1;	  
	  Octstr *rbody = NULL;
	  int rstatus = HTTP_OK;
	  
	  if (!(flg = is_allowed_ip(settings->admin_allow_ip, settings->admin_deny_ip, ip)) || 
	      ((pass = http_cgi_variable(cgivars, "password")) == NULL  &&
	       settings->admin_pass != NULL) ||
	      (settings->admin_pass && octstr_compare(pass, settings->admin_pass) != 0)) {
	       mms_error(0,  "mmsc", NULL, "Improper access to mmsbox admin interface from IP[%s]: %s",
			 octstr_get_cstr(ip),
			 flg ? "Invalid/empty password" : "IP not allowed");
	       if (flg) {/* means it is allowed by IP */
		    rstatus = HTTP_UNAUTHORIZED;
		    rbody = octstr_imm("<error>Auth failed</error>");
	       }
	  } else {
	       Octstr *vasp_id = http_cgi_variable(cgivars, "vasp-id");	       
	       List *l = NULL;
	       
	       rbody = octstr_create("<?xml version='1.0'?>\n<mmsc>\n");
	       /* Command URI is one of: /status, /start, /stop.
		* vasp-id is either empty (meaning ALL) or an ID of an existing VASPs connection.
		*/
	       
	       if (octstr_str_case_compare(url, "/start") == 0) {
		    mCfgGrp *m;
		    if (vasp_id == NULL)
			 l = mms_cfg_get_multi(settings->cfg, octstr_imm("mms-vasp"));
		    else if ((m = mms_get_multi_by_field(settings->cfg, 
							 octstr_imm("mms-vasp"), 
							 octstr_imm("vasp-id"), 
							 vasp_id)) != NULL) {
			 l = gwlist_create();
			 gwlist_append(l, m);
		    }
		    /* Start MMS VASPs. */
		    
		    while ((m = gwlist_extract_first(l)) != NULL) {
			 Octstr *x;
			 List *e = gwlist_create();
			 List *w = gwlist_create();			 
			 MmsVasp *mv = mmsc_load_vasp_from_conf(settings, m, e, w);
			 
			 if (mv != NULL)
			      append_vasp_status(rbody, mv, w);
			 else if (gwlist_len(e) > 0)
			      while ((x = gwlist_extract_first(e)) != NULL) {
				   octstr_format_append(rbody, 
							"<load-vasp><error>%S</error></load-vasp>\n",
							x);				   
				   octstr_destroy(x);
			      }
			 gwlist_destroy(e, (void *)octstr_destroy);
			 gwlist_destroy(w, (void *)octstr_destroy);

			 mms_cfg_destroy_grp(settings->cfg, m);
		    }		    
	       } else if (octstr_str_case_compare(url, "/stop") == 0) {
		    Octstr *x;
		    if (vasp_id == NULL)
			 l = dict_keys(settings->vasp_list);
		    else {
			 l = gwlist_create();
			 gwlist_append(l, octstr_duplicate(vasp_id));
		    }

		    while ((x = gwlist_extract_first(l)) != NULL) {
			 int ret = mmsc_unload_vasp(settings, x);
			 octstr_format_append(rbody, 
					      "<unload-vasp><%s/></unload-vasp>\n",
					      ret == 0 ? "Success" : "Failed");
			 octstr_destroy(x);
		    }					      
	       } else if (octstr_str_case_compare(url, "/status") == 0) {
		    Octstr *x;
		    MmsVasp *mv;
		    if (vasp_id == NULL)
			 l = dict_keys(settings->vasp_list);
		    else {
			 l = gwlist_create();
			 gwlist_append(l, octstr_duplicate(vasp_id));
		    }

		    while ((x = gwlist_extract_first(l)) != NULL) {
			 if ((mv = dict_get(settings->vasp_list, x)) != NULL)
			      append_vasp_status(rbody, mv, NULL);
			 octstr_destroy(x);
		    }
	       }	       
	       gwlist_destroy(l, NULL);

	       octstr_append_cstr(rbody, "\n</mmsc>\n");
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

     mms_info(0,  "mmsbox", NULL,"Admin Interface -- shuttind down on port %d", (int)settings->admin_port);
}

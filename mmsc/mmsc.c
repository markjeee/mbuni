/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSC: Full MMSC startup
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <signal.h>
#include "mmsc.h"
#include "mms_uaprof.h"

MmscSettings *settings;
List *proxyrelays;

static void quit_now(int notused)
{
     stop_mmsrelay();
     stop_mmsproxy();
}

/* manage the SIGHUP signal */
static void relog_now(int notused)
{
     mms_warning(0, "mmsc", NULL, "SIGHUP received, catching and re-opening logs");
     log_reopen();
     alog_reopen();
}

int main(int argc, char *argv[])
{
     int cfidx;
     Octstr *fname;
     long r_thread = 0;

     mms_lib_init();

     srandom(time(NULL));

     cfidx = get_and_set_debugs(argc, argv, NULL);

     if (argv[cfidx] == NULL)
	  fname = octstr_imm("mbuni.conf");
     else 
	  fname = octstr_create(argv[cfidx]);


     mms_info(0,  "mmsc", NULL, "----------------------------------------");
     mms_info(0,  "mmsc", NULL," " MM_NAME " MMSC  version %s starting", MMSC_VERSION);
     
     
     settings = mms_load_mmsc_settings(fname,&proxyrelays, 0);          

     octstr_destroy(fname);     
     if (!settings) 
	  panic(0, "No MMSC configuration!");

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
     signal(SIGPIPE,SIG_IGN); /* Ignore pipe errors. They kill us sometimes for no reason*/


     mms_info(0,  "mmsc", NULL," " MM_NAME " MMSC  services:%s%s%s%s", 
	      (settings->svc_list & SvcMM1) ? " MM1" : "",
	      (settings->svc_list & SvcMM7) ? " MM7" : "",
	      (settings->svc_list & SvcRelay) ? " Relay" : "",
	      (settings->svc_list & (SvcMM1 | SvcMM7 | SvcRelay)) ? "" : " None");
     mms_info(0,  "mmsc", NULL, "----------------------------------------");
	  
     if ((r_thread = gwthread_create((gwthread_func_t *)mmsrelay, NULL)) < 0)
	  panic(0, "Failed to start MMSC Relay component!");
     
     if (mmsproxy() < 0)
	  panic(0, "Failed to start MMSC Relay component!");
     
     /* We are done. Cleanup. */
     gwthread_join(r_thread);

     mms_info(0,  "mmsc", NULL, "MMSC shutdown commenced.");
     
     gwthread_sleep(2); /* Wait for them to die. */
     mms_info(0,  "mmsc", NULL, "Final cleanup...");
     mms_cleanup_mmsc_settings(settings); /* Stop settings stuff and so on. */

     mms_info(0,  "mmsc", NULL,"Shutdown complete...");
     mms_lib_shutdown();
    
     return 0;
}

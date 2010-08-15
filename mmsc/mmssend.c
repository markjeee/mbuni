/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS send: Inject message into queue for delivery.
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include "mms_queue.h"
#include "mms_uaprof.h"
#include "mmsc_cfg.h"
#include "mms_mmbox.h"


static  Octstr *from;
static Octstr *binfo;
static List *to;
static Octstr *data;
static int savetommbox;
static int dlr;
static int find_own(int i, int argc, char *argv[])
{

     if (argv[i][1] == 'f')
	  if (i + 1  < argc) {
	       from = octstr_create(argv[i+1]);
	       return 1;
	  } else
	       return -1;
     else if (argv[i][1] == 'b') {
	  savetommbox = 1;
	  return 0;
    } else if (argv[i][1] == 't')
	 if (i + 1  < argc) {
	      Octstr *x = octstr_create(argv[i+1]);
	      to  = octstr_split(x,  octstr_imm(":"));
	      octstr_destroy(x);
	      return 1;
	  } else
	       return -1;
     else if (argv[i][1] == 'm')
	  if (i + 1  < argc) {	       
	       data = octstr_read_file(argv[i+1]);    
	       return 1;
	  } else
	       return -1;
     else if (argv[i][1] == 'r') {
	  dlr = 1;
	  return 0;
     } else if (argv[i][1] == 'B')
          if (i + 1  < argc) {
               binfo = octstr_create(argv[i+1]);
               return 1;
	  } else 	       
	       return -1;
     else 
	  return -1;
}

static MmscSettings *settings;
static List *proxyrelays;
static   MmsMsg *m;
Octstr *mmbox;

int main(int argc, char *argv[])
{
     Octstr *fname, *s;
     
     int cfidx;
     int msize;

     List *h = NULL;

     if (argc < 2)
	  return -1;
     
     mms_lib_init();

     cfidx = get_and_set_debugs(argc, argv, find_own);
     
     if (argv[cfidx] == NULL)
	  fname = octstr_imm("mmsc.conf");
     else 
	  fname = octstr_create(argv[cfidx]);

     mms_info(0, "mmssend", NULL, "----------------------------------------");
     mms_info(0,  "mmssend", NULL, " MMSC Message sender runner  version %s starting", MMSC_VERSION);
     
     
     /* Load settings. */     
     settings = mms_load_mmsc_settings(fname, &proxyrelays,1);          
    
     if (!settings) 
	  panic(0, "No global MMSC configuration, or failed to read conf from <%s>!", octstr_get_cstr(fname));

     octstr_destroy(fname);
     if (from == NULL ||
	 to == NULL) {
	  mms_error(0, "mmssend", NULL, "Sender and recipient addresses required!\n");
	  exit(-1);
     } else { /* fix up 'to' list */
	  List *l = gwlist_create();
	  Octstr *x;
	  while ((x = gwlist_extract_first(to)) != NULL) {	       
	       octstr_strip_blanks(x);
	       _mms_fixup_address(&x, 
				  settings->unified_prefix ? octstr_get_cstr(settings->unified_prefix) : NULL,
				  settings->strip_prefixes, 1);
	       gwlist_append(l, x);
	  }
	  gwlist_destroy(to, NULL);
	  to = l;
     }

     /* fix from address. */
     _mms_fixup_address(&from,  
			settings->unified_prefix ? octstr_get_cstr(settings->unified_prefix) : NULL,
			settings->strip_prefixes, 1);

#if 0
     mms_start_profile_engine(octstr_get_cstr(settings->ua_profile_cache_dir));
#endif 
     if (data) {
	  /* try and detect if we are looking at plain text (mime-encoded) or binary encoded message. */
	  int ch = octstr_get_char(data, 0);
	  if (isprint(ch)) {
		    MIMEEntity *mime = mime_octstr_to_entity(data);

		    if (mime) {
			 m = mms_frommime(mime);
			 mime_entity_destroy(mime);
		    }
	  } else 	       
	       m = mms_frombinary_ex(data, from ? from : octstr_imm("anon@anon"),
				     octstr_get_cstr(settings->unified_prefix), 
				     settings->strip_prefixes);
	  if (m) 
	       mms_msgdump(m,1);       
	  msize = octstr_len(data);
	  octstr_destroy(data);
     } else
	     msize = 0;
     if (!m)
	     panic(0, "No Message supplied, or failed to decode binary data!");
     

     h = http_create_empty_headers();
     http_header_add(h, "X-Mbuni-Tool", "mmssend");
     http_header_add(h, "X-Mbuni-CalledFrom", "Terminal");

     if (binfo) {
             mms_info(0, "add.info", NULL, "Adding extra headers billing info `X-Mms-Binfo' :");
             http_header_add(h, "X-Mms-Binfo", octstr_get_cstr(binfo));
     }
     s = settings->qfs->mms_queue_add(from, to, NULL, NULL, NULL, time(NULL), 
				      time(NULL) + settings->default_msgexpiry, m,
				      NULL, 
				      NULL, NULL,
				      NULL, NULL,
				      h,
				      dlr, 
				      octstr_get_cstr(settings->global_queuedir), 
				      "MM3",
				      settings->host_alias);
     
     if (savetommbox) 
	  mmbox = mms_mmbox_addmsg(octstr_get_cstr(settings->mmbox_rootdir),
				   octstr_get_cstr(from), m, NULL, octstr_imm("Sent"));
     
     mms_log("Received", from, to, msize, s, NULL, NULL, "MM3",NULL,NULL);
     
     printf("Queued: %s, mmbox=%s\n", 
	    octstr_get_cstr(s), mmbox ? octstr_get_cstr(mmbox) : "");
     octstr_destroy(s);
     
     http_destroy_headers(h);
     mms_cleanup_mmsc_settings(settings);
     mms_lib_shutdown();
  return 0;
}

/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Queue management functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#ifndef _MMS_QUEUE_INCLUDED__
#define _MMS_QUEUE_INCLUDED__

#include <signal.h>

#include "mms_msg.h"
#include "mms_util.h"

#define QFNAMEMAX 128

typedef struct MmsEnvelopeTo {
     Octstr *rcpt; /* Recipient address. */
     int process;     /* 1 if delivery to this recipient should be attempted. 
		       * Flags below for details. 
		       */
     enum {SFailed=0, SSuccess, SDefered, SUnknown} flag;
} MmsEnvelopeTo;

typedef struct MmsEnvelope {
     int msgtype;       /* type of message. */ 
     Octstr *msgId;     /* message id (for reference). */
     Octstr *token;     /* User level token, may be null. */ 
     Octstr *from;      /* from address. */
     Octstr *mclass;    /* Message class. */
     
     Octstr *vaspid;    /* VASPID (if any) */
     Octstr *vasid;     /* VASID (if any) */
     
     Octstr *url1;      /* Generic URLs (2) associated with message. */
     Octstr *url2;
     List   *hdrs;      /* Generic list of headers associated with message. */
     
     List   *to;        /* List of recipients: MmsEnvelopeTo (if process=0 then already sent to)*/

     Octstr *subject;   /* Message subject (if any). */

     time_t created;    /* date/time this queue entry was made. */
     time_t sendt;      /* date/time attempt should be made to send this message.*/
     time_t lasttry;    /* date/time this queue item was last run. */
     time_t expiryt;    /* date/time when this message expires. */
     time_t lastaccess;  /* Last fetch of the corresponding data. */
  
     int  dlr;          /* Whether to send delivery receipts or not. */

     long attempts;     /* Delivery attempts made so far. */

     unsigned long msize; /* Size of message in octets. */

     struct {
	  int billed;
	  double amt;
     } bill;             /* Whether this has been billed and how much. */

     Octstr *mdata;      /* Generic string data used by any interface. */
     Octstr *fromproxy;  /* Which proxy sent us this message.*/
     Octstr *viaproxy;   /* Which proxy must we send this message through. */
     
     char   src_interface[16]; /* source interface of this message. */
     char   _extra_space;                /* A bit of sugar, just in case... */

     void   *_x;        /* Generic storage field used by module clients. */
     void   *qfs_data;   /* Queue handler module structure -- allocated for you by queue_create
			 * function.
			 */
     char xqfname[64+QFNAMEMAX];      /* The full ID for the queue. Use this. */
} MmsEnvelope;

typedef struct MmsQueueHandlerFuncs {
/* Initialise queue module. Must be called at least once on each queue dir. 
 * max_concurrent is a best guess as to number of concurrent queue requests 
 */
     int (*mms_init_queue_module)(Octstr *init_data, char *top_storage_dir, int max_concurrent);
     
     /* initialise a queue directory. There can be multiple directories, 
      * upperlevel decides what a directory is.
      * module returns a directory string.
      */
     Octstr *(*mms_init_queue_dir)(char *qdir, int *error);
  
     /* cleanup module, e.g. on exit. */
     int (*mms_cleanup_queue_module)(void);
/*
 * Add a message to the queue, returns 0 on success -1 otherwise (error is logged). 
 * 'to' is a list of Octstr * *.  
 * Returns a queue file name.
 */
     Octstr *(*mms_queue_add)(Octstr  *from, List *to,
			      Octstr *subject,
			      Octstr *fromproxy, Octstr *viaproxy, 
			      time_t senddate, time_t expirydate, MmsMsg *m, 
			      Octstr *token, 
			      Octstr *vaspid, Octstr *vasid,
			      Octstr *url1, Octstr *url2,
			      List *hdrs,
			      int dlr,
			      char *directory, char *src_interface,
			      Octstr *mmscname);
     
/* 
 * Update queue status. Returns -1 on error, 0 if queue is updated fine and 
 * envelope is still valid, 1 if envelope is no longer valid (no more recipients.)
 */
     int (*mms_queue_update)(MmsEnvelope *e);

/*
 * Get the message associated with this queue entry.
 */
     MmsMsg *(*mms_queue_getdata)(MmsEnvelope *e);

/* Replace data for this queue item -- used by mm7 interface. */
     int (*mms_queue_replacedata)(MmsEnvelope *e, MmsMsg *m);

/* 
 * Reads queue, returns up to lim queue entries that are ready for processing. send 0 for no limit.
 */

/* 
 * Attempt to read an envelope from queue file:
 * - opens and locks the file. 
 * - if the lock succeeds, check that file hasn't changed since opening. If it has
 *   return NULL (i.e. file is being processed elsewhere -- race condition), otherwise read it.
 * - If should block is 1, then does a potentially blocking attempt to lock the file.
 */

     MmsEnvelope *(*mms_queue_readenvelope)(char *qf, char *dir, int shouldblock);

/*  
 * Run the queue in the given directory. For each envelope that is due for sending, call
 * deliver(). If deliver() returns 0, then queue_run needs to destroy envelope 
 * structure it passed to deliver()
 * if deliver() returns 1, it has deleted envelope. 
 * Also if rstop becomes true, queue run must stop.
 */
     void (*mms_queue_run)(char *dir, 
			int (*deliver)(MmsEnvelope *), 
			double sleepsecs,
			int num_threads,
			volatile sig_atomic_t *rstop);

/* Get rid of memory used by this. Typically does internal cleanup then calls 
 * the general structure free-ing function below. 
 */
     int (*mms_queue_free_env)(MmsEnvelope *e);     
} MmsQueueHandlerFuncs;

/* Module must export this symbol:
 * extern MmsQueueHandlerFuncs qfuncs;
 */

extern MmsQueueHandlerFuncs default_qfuncs; /* default queue handler module, file-based */
/* Utility functions, generally defined. */
/* Creates the queue envelope object, returns it.  Returns the binary MMS in 'binary_mms' if NOT NULL */
MmsEnvelope *mms_queue_create_envelope(Octstr *from, List *to, 
				       Octstr *subject,
				       Octstr *fromproxy, Octstr *viaproxy, 
				       time_t senddate, time_t expirydate,
				       Octstr *token, 
				       Octstr *vaspid, Octstr *vasid,
				       Octstr *url1, Octstr *url2,
				       List *hdrs,
				       int dlr,
				       Octstr *mmscname,
				       MmsMsg *m, 
				       char *xqfname,
				       char *src_interface,
				       int extra_space,
				       Octstr **binary_mms);
/* Get rid of memory used by this. */
void mms_queue_free_envelope(MmsEnvelope *e);

/* utility function for 'cleaning up' addresses. */
Octstr *copy_and_clean_address(Octstr *addr);
#endif

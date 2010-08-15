/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Event logger module
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */ 
#ifndef __MMS_EVENTLOGGER__INCLUDED__
#define __MMS_EVENTLOGGER__INCLUDED__

#include <stdarg.h>
#include "gwlib/gwlib.h"

typedef enum mbuni_event_type_t {MBUNI_ERROR, MBUNI_INFO, MBUNI_WARNING} mbuni_event_type_t;
typedef struct MmsEventLoggerFuncs {
     int (*init)(Octstr *init_param);
     void (*log_event)(enum mbuni_event_type_t type, const char *subtype, int err, const char *file, 
		       int line,
		       const char *function,
		       const char *interface, Octstr *id,
		       Octstr *msg);
     int (*cleanup)(void);
} MmsEventLoggerFuncs;
/* A module should expose a module of the above type with name: logger */

extern int mms_event_logger_init(MmsEventLoggerFuncs *funcs, Octstr *init_param);
/* logger function:
 * - interface is one of MM1, MM4, etc.
 * - id is an id for the interface element (e.g. mmsc id)
 */
extern void mms_event_logger_cleanup(void);
extern void mms_event_logger(enum mbuni_event_type_t type, const char *subtype, 
			     int err, const char *file, 
			     int line, const char *function, 
			     char *interface, Octstr *id,
			     char *fmt,...);

#define mms_error(err,intf,id,fmt,...) mms_event_logger(MBUNI_ERROR, NULL, (err), __FILE__,  __LINE__,__FUNCTION__,(intf), (id),(fmt),##__VA_ARGS__)
#define mms_info(err,intf,id,fmt,...) mms_event_logger(MBUNI_INFO,  NULL, (err), __FILE__,  __LINE__,__FUNCTION__,(intf), (id),(fmt),##__VA_ARGS__)
#define mms_warning(err,intf,id,fmt,...) mms_event_logger(MBUNI_WARNING,  NULL, (err), __FILE__, __LINE__,__FUNCTION__,(intf), (id),(fmt),##__VA_ARGS__)

#define mms_error_ex(subtype,err,intf,id,fmt,...) mms_event_logger(MBUNI_ERROR, (subtype), (err), __FILE__,  __LINE__,__FUNCTION__,(intf), (id),(fmt),##__VA_ARGS__)
#define mms_info_ex(subtype,err,intf,id,fmt,...) mms_event_logger(MBUNI_INFO,  (subtype), (err), __FILE__,  __LINE__,__FUNCTION__,(intf), (id),(fmt),##__VA_ARGS__)
#define mms_warning_ex(subtype,err,intf,id,fmt,...) mms_event_logger(MBUNI_WARNING,  (subtype), (err), __FILE__, __LINE__,__FUNCTION__,(intf), (id),(fmt),##__VA_ARGS__)


MmsEventLoggerFuncs shell_event_logger; /* For logging using a shell command. */

/* Stop all from using gwlib info, error and warning functions */
#define error use_mms_error_instead
#define warning use_mms_warning_instead
#define info use_mms_info_instead

#endif

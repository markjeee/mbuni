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
#include "mms_eventlogger.h"
#include "mms_util.h"

static Octstr *shell_cmd;

/* So that below compiles fine. */
#undef error
#undef warning
#undef info
static void default_logger(enum mbuni_event_type_t type, const char *subtype, int err, const char *file, 
			   int line, 
			   const char *function,
			   const char *interface, Octstr *id,
			   Octstr *msg)
{
     void (*f)(int, const char *,...);
     switch(type) {
     case MBUNI_INFO:
	  f = info;
	  break;
     case MBUNI_WARNING:
	  f = warning;
	  break;	  
     default: /* including error. */
	  f = error;
	  break;
     }
     
     f(err, "%s:%d <%s> [%s] [%s] %s", 
       file, line, function, 
       interface ? interface : "n/a", 
       id ? octstr_get_cstr(id) : "n/a",
       octstr_get_cstr(msg));	  
}


static void shell_logger(enum mbuni_event_type_t type, const char *subtype, int err, const char *file, 
			 int line, const char *function,
			 const char *interface, Octstr *id,
			 Octstr *msg)
{
     char *xtype;
     Octstr *cmd, *xid = octstr_duplicate(id);
     
     gw_assert(shell_cmd);
     
     switch(type) {
     case MBUNI_INFO:
	  xtype = "INFO";
	  break;
     case MBUNI_WARNING:
	  xtype = "WARNING";
	  break;
	  
     default: /* including error. */
	  xtype = "ERROR";
	  break;
     }

     escape_shell_chars(msg);
     escape_shell_chars(id);

     
     cmd = octstr_format("%S '%s' '%s' %d '%s' '%s' %d '%S' '%S'", 
			 shell_cmd, xtype, subtype ? subtype : "", err, interface ? interface : "", file, line, xid ? xid : octstr_imm(""), msg);
     system(octstr_get_cstr(cmd));

     octstr_destroy(cmd);
     octstr_destroy(xid);
}

static int init_default_logger(Octstr *param)
{     
     return 0;
}

static int init_shell_logger(Octstr *cmd)
{    
     shell_cmd = octstr_duplicate(cmd);
     return 0;
}


static int cleanup_shell_logger(void)
{    
     octstr_destroy(shell_cmd);
     shell_cmd = NULL;
     return 0;
}



static MmsEventLoggerFuncs default_handler = {init_default_logger, default_logger}, *log_handler = &default_handler;

MmsEventLoggerFuncs shell_event_logger = {
     init_shell_logger,
     shell_logger,
     cleanup_shell_logger
};

int mms_event_logger_init(MmsEventLoggerFuncs *funcs, Octstr *init_param)
{
     if (funcs && (funcs->init == NULL || 
		   funcs->init(init_param) == 0)) {
	  log_handler = funcs;

	  return 0;
     }

     return -1;
}

extern void mms_event_logger(enum mbuni_event_type_t type, const char *subtype, int err, const char *file, 
			     int line, const char *function, char *interface, Octstr *id,
			     char *fmt,...)
{
     Octstr *x;
     va_list ap;

     va_start(ap, fmt);
     x = octstr_format_valist(fmt, ap);

     va_end(ap);
     
     log_handler->log_event(type,subtype, err, file, line, function, interface, id, x);

     octstr_destroy(x);
}

void  mms_event_logger_cleanup(void)
{
     if (log_handler->cleanup)
	  log_handler->cleanup();      
}


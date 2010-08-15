/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Misc. functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */ 
#ifndef __MMS_UTIL__INCLUDED__
#define __MMS_UTIL__INCLUDED__

#include "gwlib/gwlib.h"
#include "gwlib/mime.h"
#include "gwlib/regex.h"

#include "mbuni-config.h"
#include "mms_strings.h"
#include "mms_msg.h"
#include "mms_mm7soap.h"
#include "mms_cfg.h"
#include "mms_mmbox.h"
#include "mms_eventlogger.h"

/* supported 3GPP MMS version */
#define MAKE_VERSION(a,b,c) ((a)<<16 | (b)<<8 | (c))

#define MAJOR_VERSION(v) (((v)>>16)&0xF)
#define MINOR1_VERSION(v) (((v)>>8)&0xF)
#define MINOR2_VERSION(v) ((v)&0xF)

#define MMS_3GPP_VERSION MAKE_VERSION(5,5,0)

/* Send errors */
#define MMS_SEND_OK 0
#define MMS_SEND_QUEUED 1
#define MMS_SEND_ERROR_TRANSIENT -1
#define MMS_SEND_ERROR_FATAL -2

/* Aging of old (internal) records... */
#define DEFAULT_DELETE_AGE 10*60

#define SEND_ERROR_STR(e) ((e) == MMS_SEND_OK ? "Sent" : \
             (e) == MMS_SEND_ERROR_TRANSIENT ? "Retry later" : \
             ((e) == MMS_SEND_QUEUED ? "Queued" : "Failed"))
/* Useful headers. */
#define XMSISDN_HEADER  "X-WAP-Network-Client-MSISDN"
#define XIP_HEADER  "X-WAP-Network-Client-IP"
#define MM_NAME "Mbuni"

#define EAIF_VERSION "%d.%d"

/* used by mmbox and queue code -- directory stuff. */
#define _TT "0123456789abcdefghijklmnopqrstuvwxyz"
#define _TTSIZE (-1 + sizeof _TT)

#define MBUNI_MULTIPART_TYPE "application/vnd.mbuni.url-list"

/* Global variables and shared code used by all modules. */

#define MMSC_VERSION MBUNI_VERSION
#define MMS_NAME PACKAGE

#define DRM_CONTENT_TYPE(ctype) ((ctype) && octstr_case_search(ctype, octstr_imm("application/vnd.oma.drm"), 0) == 0)

/* Message location flags: Used to distinguish fetch-urls */
enum mms_loc_t {MMS_LOC_MMBOX = 1, MMS_LOC_MQUEUE=2};


extern int mms_load_core_settings(mCfg *cfg, mCfgGrp *cgrp);

extern Octstr *mms_maketransid(char *qf, Octstr *mmscname);

extern Octstr *mms_make_msgid(char *qf, Octstr *mmscname);

extern Octstr *mms_getqf_fromtransid(Octstr *transid);
extern Octstr *mms_getqf_from_msgid(Octstr *msgid);

extern Octstr *mms_isodate(time_t t);
void mms_lib_init(void);
void mms_lib_shutdown(void);

/* get content type while stripping params. If any param is null, fails. */
int get_content_type(List *hdrs, Octstr **type, Octstr **params);

/* Takes a header value, returns the value proper and any parameters... */
int split_header_value(Octstr *value, Octstr **base_value, Octstr **params);
/* Returns a list of parameters as http_headers given the semi-comma delimited string.*/
List  *get_value_parameters(Octstr *params);

Octstr *make_value_parameters(List *params);

/* Where value is comma-separated, make separate header items. */
void unpack_mimeheaders(MIMEEntity *m);

/* Where element has base64 encoding, decode. */
void unbase64_mimeparts(MIMEEntity *m);

/* Where element contains binary data, encode it base64. Set all = 1 to ignore whether body is binary
 * and should be coded. 
 */
void base64_mimeparts(MIMEEntity *m, int all);


/* Send this message to email recipient: Returns 0 on success 1 or 2 on profile error 
 * (negate to get right one), -ve on some other error
 */
int mms_sendtoemail(Octstr *from, Octstr *to, 
		    Octstr *subject, Octstr *msgid,
		    MmsMsg *msg, int dlr, Octstr **error,
		    char *sendmail_cmd, Octstr *myhostname, 
		    int trans_msg,
		    int trans_smil, char *txt, char *html, int mm4,
		    char *transid, List *extra_headers);

/* Send directly to email. */
int mm_send_to_email(Octstr *to, Octstr *from, Octstr *subject, 
		     Octstr *msgid,
		     MIMEEntity *m, int append_hostname, Octstr **error, 
		     char *sendmail_cmd, Octstr *myhostname);

/* log to access log. */
void mms_log(char *logmsg, Octstr *from, List *to, 
	     int msize, Octstr *msgid,
	     Octstr *acct,
	     Octstr *viaproxy,
	     char *interface, 
	     Octstr *ua, Octstr *mmboxloc);

void mms_log2(char *logmsg, Octstr *from, Octstr *to, 
	      int msize, Octstr *msgid,
	      Octstr *acct,
	      Octstr *viaproxy,
	      char *interface, 
	      Octstr *ua, Octstr *mmboxloc);

/*
 * lockfile: tries to lock a file, returns 0 if success, errno (which could be +ve) otherwise.
 * we use flock()
 */
int mm_lockfile(int fd, char *fname, int shouldblock);


/* Returns true if the character is printable or space */
int _mms_gw_isprint(int c);

int lockfile(int fd, int shouldblock);
/*
 * unlock_and_fclose/unlock_and_close are wrappers around fclose/close
 * needed to maintain the state of the global list on Solaris.
 */
int unlock_and_fclose(FILE *fp);
int unlock_and_close(int fd);

/* Special form of cfg_get which returns zero-length string when there is nothing. */
Octstr *_mms_cfg_getx(mCfg *cfg, mCfgGrp *grp, Octstr *item);

/* Get envelope data from message headers. */
void mms_collect_envdata_from_msgheaders(List *mh, List **xto, 
					 Octstr **subject, 
					 Octstr **otransid, time_t *expiryt, 
					 time_t *deliveryt, long default_msgexpiry,
					 long max_msgexpiry,
					 char *unified_prefix, List *strip_prefixes);

/* Simple hash function */
unsigned long _mshash(char *s);

/* Tell us whether address is a phone number. */
int isphonenum(Octstr *s);
/* Fixup an address: Normalize number (if prefix given), Add type (if keep_suffix is set), etc. */
void _mms_fixup_address(Octstr **address, char *unified_prefix, List *strip_prefixes, int keep_suffix);

/* normalize a phone number: Strip any prefixes, then normalize. */
void mms_normalize_phonenum(Octstr **num, char *unified_prefix, List *strip_prefixes);

/* Check that host is one of hosts in semi-colon separated list in host_list */
int is_allowed_host(Octstr *host, Octstr *host_list);

/* escape (backlash) special shell characters. */
void escape_shell_chars(Octstr *str);

/* Parse CGI variables out of a HTTP POST request. 
 * This function understands both standard POST and enctype multipart/form-data
 * For the latter it puts the content type of each of the variables found into 
 * cgivars_ctypes (as HTTPCGIVars where name is the field name and value is the content type)
 * cgivars argument is the cgivars as passed to us by http_accept_request. For GET
 * HTTP requests this is returned unchanged, otherwise it is augmented with the 
 * variables found.
 */
int parse_cgivars(List *request_headers, Octstr *request_body,
		  List **cgivars, List **cgivar_ctypes);

/* get content-ID header, fix: WAP decoder may leave " at beginning */
Octstr *_x_get_content_id(List *headers);

/* Remove the boundary element from a list of headers. */
void strip_boundary_element(List *headers, char *s);

/* Fetch a URL. If necessary, authenticate, etc. also understands data: url scheme. */
int mms_url_fetch_content(int method, Octstr *url, List *request_headers, 
			  Octstr *body, List **reply_headers, Octstr **reply_body);

/* check that the token is valid. */
int  mms_is_token(Octstr *token);

/* try to guess content type from file name extension. */
Octstr *filename2content_type(char *fname);

/* try to give a good extension name based on the url or content type. */
char *make_file_ext(Octstr *url, Octstr *ctype, char fext[5]);

/* return true if node has a child... */
int has_node_children(xmlNodePtr node);

/* strip non-essential headers from top-level */
void strip_non_essential_headers(MIMEEntity *mime);

/* Get phone number out of mms formatted one, and unify. */
Octstr *extract_phonenum(Octstr *num, Octstr *unified_prefix);

/* strip quotes off a quoted string. */
void strip_quoted_string(Octstr *s);

/* Make a mime entity representing a multipart/form-data HTTP request. */
MIMEEntity *make_multipart_formdata(void);

/* Add a form field to the multipart/form-data entity. */
void add_multipart_form_field(MIMEEntity *multipart, char *field_name, 
			      char *ctype, char *content_loc, 
			      Octstr *data);

/* Build a multipart message from a list of URLs. Fetch each one and add. Result is a multipart/mixed message. */
MIMEEntity *multipart_from_urls(List *url_list);

/* load a shared object, then load a symbol from it. */
void *_mms_load_module(mCfg *cfg, mCfgGrp *grp, char *config_key, char *symbolname,
		       void *shell_builtin);


#define MAXQTRIES 100
#define BACKOFF_FACTOR 5*60       /* In seconds */
#define QUEUERUN_INTERVAL 1*60   /* 1 minutes. */
#define DEFAULT_EXPIRE 3600*24*7  /* One week */

#define HTTP_REPLACE_HEADER(hdr, hname, value) do { \
      http_header_remove_all((hdr), (hname)); \
      http_header_add((hdr), (hname), (value)); \
} while (0)
#endif

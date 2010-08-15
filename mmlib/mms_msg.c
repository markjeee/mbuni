/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS message encoder/decoder and helper functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include <time.h>
#include <ctype.h>
#include "mms_msg.h"
#include "mms_util.h"

struct MmsMsg {
     int message_type; 
     Octstr *msgId;
     List *headers; /* Of type HTTPHeader. */
     mms_encoding enc;
     unsigned char ismultipart;
     union {
	  List *l;
	  Octstr *s;
     } body; /* list of MIMEEntity (for multipart), text otherwise.*/
};


#define SIZHINT 47


static int encode_msgheaders(Octstr *os, List *hdrs);
static int decode_msgheaders(ParseContext *context, List *hdr, Octstr *from, int stop_on_ctype, 
			     char *unified_prefix, List *strip_prefixes);


static inline void pack_short_integer(Octstr *s, int ch)
{
     unsigned long c = ((unsigned)ch)&0x7f;
     wsp_pack_short_integer(s, c);
}

#if 0
static void encode_uint(Octstr *os, unsigned int l)
{
     char xbuf[5];
     int i = 0; 
     
     do {
	  xbuf[i++] = l&0x7f;
	  l>>=7;
     } while (i < 5 && l);

     while (--i > 0) {
	  xbuf[i] |= 0x80;
	  octstr_append_data(os, &xbuf[i], 1);
     }

     octstr_append_data(os, &xbuf[0], 1);
}
#endif

int mms_validate_address(Octstr *s)
{

     int i;
     int l;
     
     if (s == NULL) 
	  return -1;

     i = octstr_search_char(s, '/', 0);
     l = octstr_len(s);
     if (octstr_search_char(s, '@', 0) > 0)
	  return 0;
     else if (i >= 0)	
	  if (octstr_case_search(s, octstr_imm("PLMN"), 0) + 4 == l ||
	      octstr_case_search(s, octstr_imm("IPv4"), 0) + 4 == l ||
	      octstr_case_search(s, octstr_imm("IPv6"), 0) + 4 == l)
	       return 0;
	  else
	       return -1;
     else
	  return -1;
     
}


static int decode_multipart(ParseContext *context, List *body)
{
     int i=0, n;
     
     n = parse_get_uintvar(context);
     
     for  (i = 0; i<n && parse_octets_left(context) > 0 ; i++) {
	  int dlen, hlen;
	  MIMEEntity *x = mime_entity_create();	 
	  List *headers;
	  
	  Octstr *hs;
	  Octstr *content;
	  Octstr *content_type;


	  hlen = parse_get_uintvar(context);
	  dlen = parse_get_uintvar(context);
	  
	  if (hlen < 0 || dlen < 0) {
	       int pleft = parse_octets_left(context);
	       mms_warning(0, NULL, NULL, "Parse error reading mime body [hlen=%d, dlen=%d, left=%d]!",
		       hlen,dlen, pleft);
	       mime_entity_destroy(x);
	       return -1;
	  }
	  
	  parse_limit(context, hlen);

	  hs = parse_get_octets(context, parse_octets_left(context));

	  headers = wsp_headers_unpack(hs, 1);
	  octstr_destroy(hs);

	  strip_boundary_element(headers,NULL);
	  mime_replace_headers(x, headers);	  
	  
	  parse_skip_to_limit(context);
	  parse_pop_limit(context);
	  
	  content_type = http_header_value(headers, octstr_imm("Content-Type"));	  
	  content = parse_get_octets(context, dlen);

	  http_destroy_headers(headers);

	  if (!content || !content_type) { 
	       int pleft = parse_octets_left(context);
	       mms_warning(0, NULL, NULL, "Parse error reading mime body [hlen=%d, dlen=%d, left=%d]!",
		       hlen,dlen, pleft);
	       mime_entity_destroy(x);
	       octstr_destroy(content_type);
	       octstr_destroy(content);
	       return -1;
	  }

	  if (octstr_case_compare(content_type, 
				 octstr_imm("application/vnd.wap.multipart.related")) == 0 ||
	      octstr_case_compare(content_type, 
				 octstr_imm("application/vnd.wap.multipart.alternative")) == 0 ||
	      octstr_case_compare(content_type, 
				 octstr_imm("application/vnd.wap.multipart.mixed")) == 0) { /* Body is multipart. */
	       ParseContext *p = parse_context_create(content);
	       List *ml =  gwlist_create();	       
	       int res = decode_multipart(p, ml);

	       parse_context_destroy(p);
	       if (res == 0) {
		    /* Put body parts into mime object. */
		    int j, m  = gwlist_len(ml);
		    for (j = 0; j < m; j++)
			 mime_entity_add_part(x, gwlist_get(ml,j));		    
	       }

	       gwlist_destroy(ml, (gwlist_item_destructor_t *)mime_entity_destroy);
	       if (res < 0) {
		 octstr_destroy(content_type);	
		 octstr_destroy(content);		    
		 return -1;
	       }
	  } else 
	       mime_entity_set_body(x,content);
	  octstr_destroy(content);
	  octstr_destroy(content_type);
	  gwlist_append(body, x);
     }
     return 0;         
}

static int encode_multipart(Octstr *os, List *body)
{
     int i, j, n, m;

     n = gwlist_len(body);
     octstr_append_uintvar(os, n);

     i = 0;
     while (i<n) {
	  Octstr *mhdr, *mbody = octstr_create("");
	  MIMEEntity *x = gwlist_get(body, i);
	  List *headers = mime_entity_headers(x);
	  Octstr *s;
	  
	  strip_boundary_element(headers,NULL);
	  mhdr = wsp_headers_pack(headers, 1, WSP_1_3);
	  http_destroy_headers(headers);

	  if ((m = mime_entity_num_parts(x)) > 0) {  /* This is a multi-part, 
						      * go down further. 
						      */
	       List *l = gwlist_create();

	       for (j = 0; j < m; j++)
		    gwlist_append(l, mime_entity_get_part(x, j));
	       encode_multipart(mbody, l);
	       gwlist_destroy(l, (gwlist_item_destructor_t *)mime_entity_destroy);
	  } else if ((s = mime_entity_body(x)) != NULL) {
	       octstr_append(mbody, s);
	       octstr_destroy(s);
	  }
	  
	  octstr_append_uintvar(os, octstr_len(mhdr));
	  octstr_append_uintvar(os, octstr_len(mbody));

	  octstr_append(os, mhdr);
	  octstr_append(os, mbody);

	  octstr_destroy(mhdr);
	  octstr_destroy(mbody);

	  i++;
     }
     return 0;         
}

static int decode_msgbody(ParseContext *context, MmsMsg *msg)
{
     int res = 0;
     if (msg->ismultipart) {
	  msg->body.l = gwlist_create();
	  res = decode_multipart(context, msg->body.l);
     } else 
	  msg->body.s = parse_get_rest(context);

     return res;     
}

static void encode_msgbody(Octstr *os, MmsMsg *msg)
{
     if (msg->ismultipart) 
	  encode_multipart(os, msg->body.l);    
     else
	  octstr_append(os, msg->body.s);
}

/* If ret < 0 then we need to get a field value, else we use what's passed. */
static Octstr *decode_encoded_string_value(int ret, ParseContext *context, unsigned char *hname)
{
     int val;
     int ret2;
     Octstr *res = NULL;
     
     ret2 = (ret < 0) ?  wsp_field_value(context, &val) : ret;
     
     if (ret2 == WSP_FIELD_VALUE_DATA) { /* expect charset text. */
	  long charset; /* Get it and ignore it. */
	  wsp_secondary_field_value(context, &charset);
	  res = parse_get_nul_string(context); /* XXX Currently we ignore charset */

	  if (ret < 0) {
	       parse_skip_to_limit(context);
	       parse_pop_limit(context);
	  }

     } else if (ret2 != WSP_FIELD_VALUE_NUL_STRING) {
	  mms_warning(0, "mms_msg", NULL, "Faulty header value for %s! [ret=%d,ret2=%d]\n", hname,ret,ret2);
	  res = octstr_imm("");
     } else 
	  res = parse_get_nul_string(context);
     
     return res;
}

/* Decodes it, adds to 'unpacked' which is the header list. 
 * returns the first value token got when parsing value -- 
 * useful for unpacking msg type 
 */
static int mms_unpack_well_known_field(List *unpacked, int field_type,
				       ParseContext *context, 
				       Octstr *xfrom, int msgtype, 
				       char *unified_prefix, List *strip_prefixes)
{
     int val, ret;
     unsigned char *hname = NULL;

     Octstr *decoded = NULL;
     unsigned char *ch = NULL;

     ret = wsp_field_value(context, &val);

     if (parse_error(context)) {
	  mms_warning(0, "mms_msg", NULL, "Faulty header [code = %d], skipping remaining headers.", field_type);
	  parse_skip_to_limit(context);
	  return -2; /* serious error, bail out */
     }

     hname = mms_header_to_cstr(field_type);

     if (ret == WSP_FIELD_VALUE_NUL_STRING) 
	  decoded = parse_get_nul_string(context);
     
     switch (field_type) {
	  
     case MMS_HEADER_TO:
     case MMS_HEADER_CC:
     case MMS_HEADER_BCC:
	  if (ret == WSP_FIELD_VALUE_DATA) 
	       decoded = decode_encoded_string_value(ret, context, hname);
	  if (decoded) 
	       _mms_fixup_address(&decoded, unified_prefix, strip_prefixes, 1);
	  if (decoded == NULL || mms_validate_address(decoded))
	       mms_warning(0, "mms_msg", NULL, "Faulty address [%s] format in field %s!", 
		       octstr_get_cstr(decoded), hname);
	  break;
     case MMS_HEADER_SUBJECT:
     case MMS_HEADER_RETRIEVE_TEXT:
	  
     case MMS_HEADER_STORE_STATUS_TEXT:
	  if (ret == WSP_FIELD_VALUE_DATA) 
	       decoded = decode_encoded_string_value(ret, context, hname);
	  break;

     case MMS_HEADER_RESPONSE_TEXT:    
	  if (msgtype == MMS_MSGTYPE_MBOX_DELETE_REQ && 
	      ret == WSP_FIELD_VALUE_DATA) {
	       int ret2;

	       decoded = wsp_unpack_integer_value(context);

	       ret2= wsp_field_value(context, &val);
	       if (decoded)
		    octstr_append(decoded, decode_encoded_string_value(ret2, context, hname));
	       else 
		    mms_warning(0, "mms_msg", NULL, "Error decoding Header-Response-Text header value");
	       if (ret2 == WSP_FIELD_VALUE_DATA) { /* we need to skip to end of inner value-data. */
		    parse_skip_to_limit(context);
		    parse_pop_limit(context);		    
	       } 
	  } else if (ret == WSP_FIELD_VALUE_DATA) 
	       decoded = decode_encoded_string_value(ret, context, hname);
    
	  break;
     case MMS_HEADER_TRANSACTION_ID:
     case MMS_HEADER_MESSAGE_ID:
     case MMS_HEADER_REPLY_CHARGING_ID:
	  if (ret != WSP_FIELD_VALUE_NUL_STRING)
	       mms_warning(0, "mms_msg", NULL, "Unexpected field value type %d for header %s\n",
		       ret, hname);
	  
	  break;	       

	  /* MMS v1.2 mandates slightly different format,
	   * when used in m-mbox-delete.conf
	   */
     case MMS_HEADER_CONTENT_LOCATION:
	  if (ret == WSP_FIELD_VALUE_DATA) {
	       Octstr *t;
	       decoded = wsp_unpack_integer_value(context);
	       t = parse_get_nul_string(context);
	       if (decoded)
		    octstr_append(decoded, t);
	       else 
		    mms_warning(0, "mms_msg", NULL, "error decoding value for header %s\n",
			    hname);

	       octstr_destroy(t);
	  } else if (ret != WSP_FIELD_VALUE_NUL_STRING)
	       mms_warning(0, "mms_msg", NULL, "Unexpected field value type %d for header %s\n",
		       ret, hname);	  
	  break;
     case MMS_HEADER_MMS_VERSION:
	  if (ret == WSP_FIELD_VALUE_ENCODED)
	       decoded = wsp_unpack_version_value(val);
	  break;
	  /* integer/octet values */
     case MMS_HEADER_DELIVERY_REPORT:	 
     case MMS_HEADER_REPORT_ALLOWED:
     case MMS_HEADER_READ_REPORT:

     case MMS_HEADER_DISTRIBUTION_INDICATOR:
     case MMS_HEADER_QUOTAS:

     case MMS_HEADER_STORE:
     case MMS_HEADER_STORED:
     case MMS_HEADER_TOTALS:
	  
	  ch = mms_reports_to_cstr(val);
	  break;
     case MMS_HEADER_MESSAGE_TYPE:
	  ch = mms_message_type_to_cstr(val);
	  break;
     case MMS_HEADER_PRIORITY:
	  ch = mms_priority_to_cstr(val);
	  break;
     case MMS_HEADER_READ_STATUS:
	  ch = mms_read_status_to_cstr(val);
	  break;	  
     case MMS_HEADER_REPLY_CHARGING:
	  ch = mms_reply_charging_to_cstr(val);
	  break;

     case MMS_HEADER_RESPONSE_STATUS:
	  if (ret == WSP_FIELD_VALUE_DATA) {
	       unsigned char *x;
	       int val;
	       decoded = wsp_unpack_integer_value(context);
	       wsp_field_value(context, &val);
	       x = mms_response_status_to_cstr(val|0x80);
	       
	       if (decoded && x)
		    octstr_append_cstr(decoded, (char *)x);
	       else 
		    mms_warning(0, "mms_msg", NULL, "error decoding field_value_data value for header %s: val=%s\n",
			    hname, x ? (char *)x : (char *)"(null)");	       
	  } else 
	       ch = mms_response_status_to_cstr(val|0x80);
	  break;
     case MMS_HEADER_RETRIEVE_STATUS:
	  ch = mms_retrieve_status_to_cstr(val|0x80);
	  break;
     case MMS_HEADER_STATUS:
	  ch = mms_status_to_cstr(val);
	  break;
     case MMS_HEADER_SENDER_VISIBILITY:
	  ch = mms_visibility_to_cstr(val);
	  break;
     case MMS_HEADER_MESSAGE_CLASS: 	  
	  if (ret != WSP_FIELD_VALUE_NUL_STRING) 
	       ch = mms_message_class_to_cstr(val);	              
	  break;
	  
     case MMS_HEADER_DATE: /* Date values. */	       
	  parse_skip(context, -1); 
	  decoded = wsp_unpack_date_value(context);
	  break;	  

     case MMS_HEADER_MESSAGE_SIZE:
     case MMS_HEADER_REPLY_CHARGING_SIZE:
     case MMS_HEADER_START:

     case MMS_HEADER_LIMIT:
     case MMS_HEADER_MESSAGE_COUNT:

	  parse_skip(context, -1); 
	  decoded = wsp_unpack_integer_value(context);
	  break;
	  
     case MMS_HEADER_CONTENT_TYPE:
	  if (ret == WSP_FIELD_VALUE_ENCODED)
	       ch  = wsp_content_type_to_cstr(val);
	  else if (ret == WSP_FIELD_VALUE_DATA) 
	       decoded = wsp_unpack_accept_general_form(context);
	  break;
	  
     case MMS_HEADER_DELIVERY_TIME:
     case MMS_HEADER_EXPIRY:
     case MMS_HEADER_REPLY_CHARGING_DEADLINE:	  	  
	  if (ret != WSP_FIELD_VALUE_DATA) 
	       mms_warning(0, "mms_msg", NULL, "Error in value format, field %s!",
			    hname);
	  else {
	       int n = parse_get_char(context);
	       
	       if (n == 0x80) /* Absolute time */
		    decoded = wsp_unpack_date_value(context);
	       else if (n == 0x81) { /* Relative time. */			 
		    long l = 0;
		    time_t t = time(NULL);
		    Octstr *s = wsp_unpack_integer_value(context);
		    
		    octstr_parse_long(&l, s, 0, 10);
		    octstr_destroy(s);			 
		    
		    t += l;
		    decoded = date_format_http(t);			 
	       } else
		    decoded = date_format_http(time(NULL)); /* A default. */
	  }	       
	  break;
     case MMS_HEADER_FROM:
	       if (ret != WSP_FIELD_VALUE_DATA) 
		    mms_warning(0, "mms_msg", NULL, "Error in value format, field %s!",hname);
	       
	       else {	       
		    int n = parse_get_char(context);
		    
		    if (n == 0x80)  /* Address present. */
			 decoded = decode_encoded_string_value(-1,context, hname);
		    else /* Insert address. */
			 decoded = octstr_duplicate(xfrom);
		    
		    if (decoded)
			 _mms_fixup_address(&decoded, unified_prefix, strip_prefixes, 1);
		    if (decoded == NULL || mms_validate_address(decoded))
			 mms_warning(0, "mms_msg", NULL, "Faulty address [%s] format in field %s!", 
				 octstr_get_cstr(decoded), hname);			 
	       }
	       break;
     case MMS_HEADER_PREVIOUSLY_SENT_BY:
     case MMS_HEADER_PREVIOUSLY_SENT_DATE:
	  if (ret != WSP_FIELD_VALUE_DATA) 
	       mms_warning(0, "mms_msg", NULL, "Error in value format, field %s!",hname);
	  else {
	       Octstr *t;
	       decoded = wsp_unpack_integer_value(context);
	       
	       if (field_type == MMS_HEADER_PREVIOUSLY_SENT_BY) 
		    t = decode_encoded_string_value(-1, context, hname);
	       else
		    t = wsp_unpack_date_value(context);
	       if (decoded)
		    octstr_append(decoded, t);
	       else 
		    mms_warning(0, "mms_msg", NULL, "error decoding value for header  %s!", 
			    hname);			 
	       octstr_destroy(t);
	  }
	  break;  
     /* From here on, these are MMS v1.2 thingies. a few are above as well... */
     case MMS_HEADER_ATTRIBUTES:
	  ch = mms_header_to_cstr(val);
	  break;

     case MMS_HEADER_ELEMENT_DESCRIPTOR:
	  if (ret != WSP_FIELD_VALUE_DATA)
	       mms_warning(0, "mms_msg", NULL, "Faulty header value for %s!\n", hname);
	  else { /* We expect a content reference and a list of parameters. */
	       Octstr *cr = parse_get_nul_string(context);
	       List *params = http_create_empty_headers();
	       Octstr *ps;
	       while (parse_octets_left(context) > 0) {
		    int val, ret;
		    Octstr *pname = NULL, *pval = NULL;
		    ret = wsp_field_value(context, &val);
		    if (ret == WSP_FIELD_VALUE_ENCODED)
			 pname = mms_descriptor_params_to_string(val|0x80);
		    else if (ret == WSP_FIELD_VALUE_NUL_STRING)
			 pname = parse_get_nul_string(context);

		    if (!pname)
			 continue;
		    ret = wsp_field_value(context, &val);
		    if (ret == WSP_FIELD_VALUE_ENCODED)
			 pval = wsp_content_type_to_string(val);
		    else if (ret == WSP_FIELD_VALUE_NUL_STRING)
			 pval = parse_get_nul_string(context);
		    
		    if (pval) {
			 http_header_add(params, octstr_get_cstr(pname), octstr_get_cstr(pval));
			 octstr_destroy(pval);
		    }		    
		    octstr_destroy(pname);		    
	       }
	       ps = make_value_parameters(params);
	       decoded = octstr_format("%S%s%S", cr, 
				       (ps && octstr_len(ps) > 0) ? "; " : "", 
				       ps);
	       octstr_destroy(cr);
	       octstr_destroy(ps);
	       http_destroy_headers(params);
	  }
	  break;

     case MMS_HEADER_MBOX_TOTALS:
     case MMS_HEADER_MBOX_QUOTAS:
	  if (ret != WSP_FIELD_VALUE_DATA)
	       mms_warning(0, "mms_msg", NULL, "Faulty header value for %s!\n", hname);
	  else {
	       int n = parse_get_char(context);
	       decoded = wsp_unpack_integer_value(context);	       
	       if (decoded)
		    octstr_format_append(decoded, " %s", (n == 0x80) ? "msgs" : "bytes");
	       else 
		    mms_warning(0, "mms_msg", NULL, "error decoding  value for header %s!\n", hname);
	  }	  
	  break;
     case MMS_HEADER_MM_FLAGS:
	  if (ret != WSP_FIELD_VALUE_DATA)
	       mms_warning(0, "mms_msg", NULL, "Faulty header value for %s!\n", hname);
	  else {
	       int n = parse_get_char(context);	 
	       char *s;
	       Octstr *p;
	       
	       if (n == 0x80) /* Add, subtract or filter. */
		    s = "+";
	       else if (n == 0x81)
		    s = "-";
	       else 
		    s = "/";
	       
	       p = decode_encoded_string_value(-1,context, hname);
	       decoded = octstr_format("%s%S", s, p);
	       octstr_destroy(p);
	  }
	  break;
     case MMS_HEADER_MM_STATE:
	  ch = mms_mm_state_to_cstr(val);
	  break;
     case MMS_HEADER_STORE_STATUS:
	  ch = mms_store_status_to_cstr(val|0x80);
	  break;
	  
     default:
	  mms_warning(0, "mms_msg", NULL, "MMS: Unknown header with code 0x%02x!", field_type);
     }

     if (ret == WSP_FIELD_VALUE_DATA) {
	  parse_skip_to_limit(context);
	  parse_pop_limit(context);
     }

     if (ch == NULL && decoded != NULL)
	  ch = (unsigned char *)octstr_get_cstr(decoded);
     
     if (ch == NULL)
	  goto value_error;
     
     if (!hname) {
	  mms_warning(0, "mms_msg", NULL, "Unknown header number 0x%02x.", field_type);
	  goto value_error;
     }
     
     http_header_add(unpacked, (char *)hname, (char *)ch);
     octstr_destroy(decoded);
     return val; /* success (we hope) */
     
 value_error:
     mms_warning(0, "mms_msg", NULL, "Skipping faulty header [code=%d, val=%d]!", field_type, val);
     octstr_destroy(decoded);     
     return -2;
}

static int decode_msgheaders(ParseContext *context, List *hdrs, Octstr *from, 
			     int stop_on_ctype, char *unified_prefixes, List *strip_prefixes)
{
     int fcont = 1;
     int msgtype = 0;
     
     gw_assert(hdrs != NULL);
     
     while (fcont && parse_octets_left(context) && 
	  !parse_error(context)) {
	  int byte = parse_get_char(context);
	  int val = 0;
	  
	  if (byte >= 0x80)
	       val = mms_unpack_well_known_field(hdrs, byte&0x7f, context, from, msgtype, 
						 unified_prefixes, strip_prefixes);
	  else if (byte >= 0) {
	       parse_skip(context, -1); /* Go back a bit. */
	       wsp_unpack_app_header(hdrs, context);
	  } 
	    
	  if (val < -1) /* serious parser error occured above. */
	       break; 

	  if ((byte&0x7f) ==  MMS_HEADER_CONTENT_TYPE && 
	       stop_on_ctype)
	       fcont = 0;
	  if ((byte&0x7f) ==  MMS_HEADER_MESSAGE_TYPE)
	       msgtype = val;
     }
     
     return 0;
}


static void mms_pack_well_known_field(Octstr *os, int field_type, Octstr *value)
{
     Octstr *encoded = octstr_create("");
     int ch = 0;
     unsigned char c;

     switch (field_type) {
	  
     case MMS_HEADER_TO:
     case MMS_HEADER_CC:
     case MMS_HEADER_BCC:
     case MMS_HEADER_SUBJECT:
     case MMS_HEADER_TRANSACTION_ID:
     case MMS_HEADER_MESSAGE_ID:
     case MMS_HEADER_REPLY_CHARGING_ID:
     case MMS_HEADER_RETRIEVE_TEXT:

     case MMS_HEADER_STORE_STATUS_TEXT:
	  
	  wsp_pack_text(os, value); /* XXX need to deal with charset issues. */
	  break;	       
	  
     case MMS_HEADER_RESPONSE_TEXT: /* make sure response status does not begin with digit!! Has special meaning*/
     case MMS_HEADER_CONTENT_LOCATION:
	  if (isdigit(octstr_get_char(value, 0))) { /* begins with number. */
	       long i, l;
	       Octstr *s;
	       i = octstr_parse_long(&l, value, 0, 10);
	       if (i < 0) {
		    mms_warning(0, "mms_msg", NULL, "Bad counter for field %s!", 
			    mms_header_to_cstr(field_type));
		    i = 0;
	       }
	       wsp_pack_integer_value(encoded, l);
	       s = octstr_copy(value, i, octstr_len(value));
	       octstr_strip_blanks(s);

	       wsp_pack_text(encoded,s);
	       wsp_pack_value(os, encoded);
	       
	       octstr_destroy(s);
	  } else
	       wsp_pack_text(os, value);
	  break;
     case MMS_HEADER_MMS_VERSION:
	  wsp_pack_version_value(os, value);
	  break;
	  /* integer/octet values: Need to stream line this with better error-checking */
     case MMS_HEADER_DELIVERY_REPORT:	 
     case MMS_HEADER_REPORT_ALLOWED:
     case MMS_HEADER_READ_REPORT:
	  
     case MMS_HEADER_DISTRIBUTION_INDICATOR:
     case MMS_HEADER_QUOTAS:
	  
     case MMS_HEADER_STORE:
     case MMS_HEADER_STORED:
     case MMS_HEADER_TOTALS:

	  pack_short_integer(os, mms_string_to_reports(value));
	  break;
     case MMS_HEADER_MESSAGE_TYPE:
	   pack_short_integer(os, mms_string_to_message_type(value));
	  break;
     case MMS_HEADER_PRIORITY:
	  pack_short_integer(os, mms_string_to_priority(value));
	  break;
     case MMS_HEADER_READ_STATUS:
	  pack_short_integer(os, mms_string_to_read_status(value));
	  break;	  
     case MMS_HEADER_REPLY_CHARGING:
	  pack_short_integer(os, mms_string_to_reply_charging(value));
	  break;

     case MMS_HEADER_RESPONSE_STATUS:
	  if (isdigit(octstr_get_char(value, 0))) { /* begins with number. */
	       long i, l;
	       Octstr *s;
	       i = octstr_parse_long(&l, value, 0, 10);
	       if (i < 0) {
		    mms_warning(0, "mms_msg", NULL, "Bad counter for field %s!", 
			    mms_header_to_cstr(field_type));
		    i = 0;
	       }
	       wsp_pack_integer_value(encoded, l);
	       s = octstr_copy(value, i, octstr_len(value));
	       octstr_strip_blanks(s);

	       pack_short_integer(encoded, mms_string_to_response_status(s));
	       wsp_pack_value(os, encoded);
	       
	       octstr_destroy(s);
	  } else
	       pack_short_integer(os, mms_string_to_response_status(value));
	  break;
     case MMS_HEADER_RETRIEVE_STATUS:
	  pack_short_integer(os, mms_string_to_retrieve_status(value));
	  break;
     case MMS_HEADER_STATUS:
	  pack_short_integer(os, mms_string_to_status(value));
	  break;
     case MMS_HEADER_SENDER_VISIBILITY:
	  pack_short_integer(os, mms_string_to_visibility(value));
	  break;
     case MMS_HEADER_MESSAGE_CLASS: 	  
	  ch = mms_string_to_message_class(value);
	  if (ch < 0)
	       wsp_pack_text(os, value);
	  else
	       pack_short_integer(os, ch);
	  break;
	  
     case MMS_HEADER_DATE: /* Date values. */	       
	  wsp_pack_date(os, value);
	  break;	  

     case MMS_HEADER_MESSAGE_SIZE:

     case MMS_HEADER_REPLY_CHARGING_SIZE:
     case MMS_HEADER_START:

     case MMS_HEADER_LIMIT:
     case MMS_HEADER_MESSAGE_COUNT:

	  wsp_pack_integer_string(os, value);
	  break;
	  
     case MMS_HEADER_CONTENT_TYPE:
	  wsp_pack_content_type(os, value);
	  break;
	  
     case MMS_HEADER_DELIVERY_TIME:
     case MMS_HEADER_EXPIRY:
     case MMS_HEADER_REPLY_CHARGING_DEADLINE:

	  if (octstr_isnum(value) == 1) { 	       /* A delta value. */

	       long l;

	       sscanf(octstr_get_cstr(value), "%ld", &l);
	       c = 129;
	       octstr_append_char(encoded, c);
	       wsp_pack_long_integer(encoded, l);
	  } else {  /* Must be a date value .*/
	       c = 128;
	       octstr_append_char(encoded, c);	       
	       wsp_pack_date(encoded, value);
	  }

	  wsp_pack_value(os, encoded);

	  break;
     case MMS_HEADER_FROM:

	  if (octstr_compare(octstr_imm("#insert"), value) == 0) {
	       c = 129;
	       octstr_append_data(encoded, (char *)&c, 1);
	  } else {
	       c = 128;
	       octstr_append_data(encoded, (char *)&c, 1);
	       wsp_pack_text(encoded, value);
	  }
	  wsp_pack_value(os, encoded);
	  
	  break;
     case MMS_HEADER_PREVIOUSLY_SENT_BY:
     case MMS_HEADER_PREVIOUSLY_SENT_DATE:
     {
	  long i, l;
	  Octstr *s;
	  
	  i = octstr_parse_long(&l, value, 0, 10);
	  if (i <0) {
	       mms_warning(0, "mms_msg", NULL, "Bad counter indicator for field!");
	       i = 0;
	  }
	  
	  wsp_pack_integer_value(encoded, l);
	  s = octstr_copy(value, i, octstr_len(value));

	  octstr_strip_blanks(s);

	  if (field_type == MMS_HEADER_PREVIOUSLY_SENT_BY) 		
	       wsp_pack_text(encoded, s);
	  else
	      wsp_pack_date(encoded, s);
	       
	  octstr_destroy(s);
	  wsp_pack_value(os, encoded);
     }
     break;     

     case MMS_HEADER_MM_STATE:
	  pack_short_integer(os, mms_string_to_mm_state(value));
	  break;
	  
     case MMS_HEADER_STORE_STATUS:
	  pack_short_integer(os, mms_string_to_store_status(value));
	  break;
     case MMS_HEADER_MM_FLAGS:
     {
	  Octstr *s;
	  int i = 1;
	  ch = octstr_get_char(value, 0);
	  
	  if (ch == '+') 
	       pack_short_integer(encoded, 0x80);
	  else if (ch == '-') 
	       pack_short_integer(encoded, 0x81);
 	  else if (ch == '/')
	       pack_short_integer(encoded, 0x82);
	  else {
	       i = 0;
	       pack_short_integer(encoded, 0x82); /* default is filter. */
	  }
	  s = octstr_copy(value, i, octstr_len(value));
	  wsp_pack_text(encoded, s);
	  wsp_pack_value(os, encoded);
	  
	  octstr_destroy(s);
     }
     break;
     
     case MMS_HEADER_ATTRIBUTES:
	  ch = mms_string_to_header(value);
	  pack_short_integer(os, ch);
	  break;
	  	  
     case MMS_HEADER_MBOX_TOTALS:
     case MMS_HEADER_MBOX_QUOTAS:
     {
	  long i, l;
	  
	  i = octstr_parse_long(&l, value, 0, 10);
	  if (i <0) {
	       mms_warning(0, "mms_msg", NULL, "Bad quota value  for field %s!", mms_header_to_cstr(field_type));
	       i = 0;
	  }
	  
	  if (octstr_case_search(value, octstr_imm("bytes"), i) < 0)
	       ch = 0x80;
	  else
	       ch = 0x81;
	  pack_short_integer(encoded, ch);	  
	  wsp_pack_integer_value(encoded, l);
	  
	  wsp_pack_value(os, encoded);
     }
     break;
     
     case MMS_HEADER_ELEMENT_DESCRIPTOR:
     {
	  Octstr *cv, *cpar;
	  List *params;
	  int i, n;
	  
	  split_header_value(value, &cv, &cpar);
	  params = get_value_parameters(cpar);

	  wsp_pack_text(encoded, cv);
	  n = gwlist_len(params);

	  for (i = 0; i<n; i++) {
	       Octstr *h, *v;
	       int ch;
	       http_header_get(params, i, &h, &v);
	       ch = mms_string_to_descriptor_params(h);
	       if (ch < 0)
		    wsp_pack_text(encoded, h);
	       else
		    pack_short_integer(encoded, ch);

	       ch = wsp_string_to_content_type(v);
	       if (ch < 0)
		    wsp_pack_text(encoded, v);
	       else 
		    pack_short_integer(encoded, ch);

	       octstr_destroy(h);
	       octstr_destroy(v);
	  }
	  	  
	  octstr_destroy(cv);
	  octstr_destroy(cpar);
	  http_destroy_headers(params);

	  wsp_pack_value(os, encoded);
     }
     break;
     default:
	  mms_warning(0, "mms_msg", NULL, "MMS: Unknown header with code 0x%02x!", field_type);
     }

     octstr_destroy(encoded);
     return;
}

static int encode_msgheaders(Octstr *os, List *hdrs)
{
     int fcont = 1;
     int i, l = gwlist_len(hdrs), mtype;
     
     Octstr *msgtype = NULL, *transid = NULL, *version = NULL, *ctype;

     strip_boundary_element(hdrs,NULL);
     /* First ensure that top headers are in place. */

     version = http_header_value(hdrs, 
				 octstr_imm("X-Mms-MMS-Version"));
     
     transid = http_header_value(hdrs, 
				 octstr_imm("X-Mms-Transaction-Id"));
     msgtype = http_header_value(hdrs, 
				 octstr_imm("X-Mms-Message-Type"));
     
     ctype = http_header_value(hdrs, 
				 octstr_imm("Content-Type"));
     
     mtype = mms_string_to_message_type(msgtype);
     
     pack_short_integer(os, MMS_HEADER_MESSAGE_TYPE);
     pack_short_integer(os, mtype);
     octstr_destroy(msgtype);
     
     if (transid) {
	  pack_short_integer(os, MMS_HEADER_TRANSACTION_ID);
	  wsp_pack_text(os, transid);
	  octstr_destroy(transid);
     }
     pack_short_integer(os, MMS_HEADER_MMS_VERSION);
     wsp_pack_version_value(os, version);
     octstr_destroy(version);
     
     /* Now pack the rest. */
     for (i = 0; fcont && i < l; i++) {
	  Octstr *field = NULL, *value = NULL;	  
	  int htype;
	  
	  http_header_get(hdrs, i, &field, &value);

	  htype = mms_string_to_header(field);

	  if (htype == MMS_HEADER_MMS_VERSION ||
	      htype == MMS_HEADER_MESSAGE_TYPE ||
	      htype == MMS_HEADER_TRANSACTION_ID ||
	      htype == MMS_HEADER_CONTENT_TYPE)
	       goto loop1;

	  if (htype < 0)
	       wsp_pack_application_header(os, field, value);
	  else {
	       pack_short_integer(os, htype);
	       mms_pack_well_known_field(os, htype, value);	       
	  }
     loop1:
	  if (field) octstr_destroy(field);
	  if (value) octstr_destroy(value);
     }
     
     if (ctype) {
	  pack_short_integer(os, MMS_HEADER_CONTENT_TYPE);
	  wsp_pack_content_type(os, ctype);
	  octstr_destroy(ctype);
     } else if (mtype == MMS_MSGTYPE_SEND_REQ || 
		mtype == MMS_MSGTYPE_RETRIEVE_CONF)
	  mms_warning(0, "mms_msg", NULL, "MMS: Content type missing in encode_headers!");
     
     return 0;

}

/* Does basic fixups on a message. */
static int fixup_msg(MmsMsg *m, Octstr *from)
{
     Octstr *ver = NULL;
     Octstr *s = NULL;
     if (!m)
	  return -1;

     ver = http_header_value(m->headers, octstr_imm("X-Mms-MMS-Version"));
     if (!ver || octstr_str_compare(ver, "1.1") <= 0) {
	  m->enc = MS_1_1;
	  if (!ver)
	       http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     } else if (octstr_str_compare(ver, "1.2") <= 0)
	  m->enc = MS_1_2;
     http_header_remove_all(m->headers, "MIME-Version");
     switch (m->message_type) {
     case MMS_MSGTYPE_SEND_REQ:
     case MMS_MSGTYPE_RETRIEVE_CONF:
     case MMS_MSGTYPE_FORWARD_REQ:
	  
	  /* Check for from. */
	  if (from && (s = http_header_value(m->headers, octstr_imm("From"))) == NULL) 
	       http_header_add(m->headers, "From", octstr_get_cstr(from));
	  else 
	       octstr_destroy(s);
	  
	  /* Check for date. */
	  
	  if ((s = http_header_value(m->headers, octstr_imm("Date"))) == NULL) {
	       Octstr *t = date_format_http(time(NULL));
	       http_header_add(m->headers, "Date", octstr_get_cstr(t));
	       octstr_destroy(t);
	  } else
	       octstr_destroy(s);
	  
	  /* check for transaction ID. */
	  if ((s = http_header_value(m->headers, octstr_imm("X-Mms-Transaction-ID"))) == NULL) {
	       if (m->message_type != MMS_MSGTYPE_RETRIEVE_CONF)
		    http_header_add(m->headers, "X-Mms-Transaction-ID", "00001");
	  } else 
	       octstr_destroy(s);
#if 0    /* This will be done elsewhere. */
	  /* Check for msgid, put in if missing. */
	  if ((s = http_header_value(m->headers, octstr_imm("Message-ID"))) == NULL) 
	       http_header_add(m->headers, "Message-ID", "00000");
	  else
	       octstr_destroy(s);
#endif 
	  /* check for content-type. */
	  if ((s = http_header_value(m->headers, octstr_imm("Content-Type"))) == NULL) {
	       char *ctype;
	       if (m->body.s == NULL || 
		   (!m->ismultipart && 
		    octstr_check_range(m->body.s, 0, 
				       octstr_len(m->body.s), _mms_gw_isprint) == 0))
		    ctype = "application/octet-stream";
	       else if (m->ismultipart)
		    ctype = "application/vnd.wap.multipart.mixed";
	       else
		    ctype = "text/plain";
	       http_header_add(m->headers, "Content-Type", ctype);
	  }  else
	       octstr_destroy(s);
	  strip_boundary_element(m->headers, NULL); /* remove top-level boundary element if any. */     
	  break;
     case MMS_MSGTYPE_SEND_CONF:
     case MMS_MSGTYPE_NOTIFICATION_IND:
     case MMS_MSGTYPE_DELIVERY_IND:
     case MMS_MSGTYPE_READ_REC_IND:
     case MMS_MSGTYPE_READ_ORIG_IND:
	  http_header_remove_all(m->headers, "Content-Type"); /* Just in case, particularly from mime! */
	  break;
     }
     octstr_destroy(ver);
     return 0;
}

mms_encoding mms_message_enc(MmsMsg *msg)
{
     gw_assert(msg);
     return msg->enc;
}

MmsMsg *mms_frombinary_ex(Octstr *msg, Octstr *from, char *unified_prefix, List *strip_prefixes)
{
     int res = 0;
     MmsMsg _m = {0}, *m = NULL;
     ParseContext *p;
     Octstr *s;
     
     if (!msg)
	  return NULL;
     
     p = parse_context_create(msg);     
     mms_strings_init(); /* Just in case. */
     
     _m.headers = gwlist_create();
     decode_msgheaders(p, _m.headers, from, 1, unified_prefix, strip_prefixes);

     if (_m.headers == NULL || 
	 gwlist_len(_m.headers) == 0) 
	  goto done;        

     /* Get the message type and also set flag for whether multipart.*/
     
     s = http_header_value(_m.headers, octstr_imm("Content-Type"));     
     if (s && 
	 octstr_search(s, octstr_imm("application/vnd.wap.multipart"), 0) == 0)
	  _m.ismultipart = 1;

     octstr_destroy(s);

     s = http_header_value(_m.headers, octstr_imm("X-Mms-Message-Type"));     
     if (s) {
	  _m.message_type = mms_string_to_message_type(s); 
	  octstr_destroy(s);
     } else 
	  goto done;	  
     
     s = http_header_value(_m.headers, octstr_imm("Message-ID"));          
     _m.msgId = s;
 
     if ((res = decode_msgbody(p, &_m)) < 0)  /* A body decode error occured. */
	  goto done;
     
	 
     m = gw_malloc(sizeof m[0]); /* all ok, copy. */
     *m = _m;     
     
     fixup_msg(m, from);

 done:
     parse_context_destroy(p);
     if (!m) { /* This means an error occurred. Delete the interim stuff. */
	  MmsMsg *msg = &_m;
	
	  if (msg->ismultipart)
	       gwlist_destroy(msg->body.l, (gwlist_item_destructor_t *)mime_entity_destroy);     
	  else
	       octstr_destroy(msg->body.s);

	  http_destroy_headers(msg->headers);	
	  octstr_destroy(msg->msgId);
     }
     return m;
}

static void _x_mime_entity_dump(MIMEEntity *x, int level, int headers_only)
{
     int i,m, ism;
     List *h;
     Octstr *body;

     ism = ((m = mime_entity_num_parts(x)) > 0);
     debug("part.dump", 0, "%sMultipart -> ", ism ? "" : "Not ");
     
     h = mime_entity_headers(x);
     strip_boundary_element(h,NULL);
     http_header_dump(h);
     http_destroy_headers(h);
     
     if (ism) 
	  for (i = 0; i<m; i++) {
	       MIMEEntity *xm = mime_entity_get_part(x, i);
	       _x_mime_entity_dump(xm, level+1, headers_only);
	       mime_entity_destroy(xm);
	  }
     else if ((body = mime_entity_body(x)) != NULL) {
	  if (!headers_only)
	       octstr_dump(body, level);	       
	  octstr_destroy(body);
     }
}

void mms_msgdump(MmsMsg *m, int headers_only)
{
     int i, n;

     if (!m)
       return;

     http_header_dump(m->headers);

     debug("mms.dump", 0, "Dumping MMS message body (%s) [%ld parts] --> ", 
	   m->ismultipart ? "mulitpart" : "not multipart", 
	   m->ismultipart ? gwlist_len(m->body.l) : 0);

     if (m->ismultipart)        
	  for (i = 0, n = gwlist_len(m->body.l); i< n; i++) {
	       MIMEEntity *x = gwlist_get(m->body.l, i);
	       debug("mms.dump", 0, "--->Message part: %d --->", i);
	       
	       _x_mime_entity_dump(x,0,headers_only);
	  }
     else if (!headers_only) 
	  octstr_dump(m->body.s, 0);
     
}

Octstr *mms_tobinary(MmsMsg *msg)
{
     Octstr *s;

     if (!msg)
	  return NULL;
     s = octstr_create("");
     encode_msgheaders(s, msg->headers);
     
     if (msg->body.s)
	  encode_msgbody(s, msg);
     return s;
}

int mms_messagetype(MmsMsg *msg)
{
     gw_assert(msg);
     return msg->message_type;
}


static int convert_mime_msg(MIMEEntity *m)
{
     int i, n, res = 0;
     Octstr *content_type = NULL, *params = NULL;
     char *s = NULL;
     List *h = mime_entity_headers(m);

     n = get_content_type(h, &content_type, &params);
     
     if (n == 0 && content_type) {
	  if (octstr_str_compare(content_type, 
				 "application/vnd.wap.multipart.related") == 0)
	       s = "multipart/related";
	  else if (octstr_str_compare(content_type, 
				      "application/vnd.wap.multipart.alternative") == 0)
	       s = "multipart/alternative";
	  else if (octstr_str_compare(content_type, 
				      "application/vnd.wap.multipart.mixed") == 0)
	       s = "multipart/mixed";
     }

     if (s) {
	  Octstr *value;
	 
	  if (params && octstr_len(params) > 0) {
	       List   *ph = get_value_parameters(params); /* unpack then re-pack them with proper quoting for mime.*/
	       Octstr *ps = make_value_parameters(ph);	       
	       value = octstr_format("%s; %S", s, params);
	       octstr_destroy(ps);
	       http_destroy_headers(ph);
	  } else 
	       value = octstr_create(s);
	  
	  http_header_remove_all(h, "Content-Type");
	  http_header_add(h, "Content-Type", octstr_get_cstr(value));
	  mime_replace_headers(m,h);

	  octstr_destroy(value);
	  res = 1;
     }

     http_destroy_headers(h);
     if ((n = mime_entity_num_parts(m)) > 0)
	  for (i = 0; i < n; i++) {
	       MIMEEntity *x = mime_entity_get_part(m, i);
	       int xres = convert_mime_msg(x);	       
	       if (xres) {
		    mime_entity_replace_part(m, i, x);
		    res = 1;
	       }
	       mime_entity_destroy(x);
	  }
     octstr_destroy(params);
     octstr_destroy(content_type);

     return res;
}

static int unconvert_mime_msg(MIMEEntity *m)
{
     int i, n, res = 0;
     Octstr *content_type, *params; 
     char *s = NULL;
     List *h = mime_entity_headers(m);

     n = get_content_type(h, &content_type, &params);
     if (n == 0 && content_type) {
	  if (octstr_case_compare(content_type, 
				 octstr_imm("multipart/related")) == 0)
	       s = "application/vnd.wap.multipart.related";
	  else if (octstr_case_compare(content_type, 
				      octstr_imm("multipart/alternative")) == 0)
	       s = "application/vnd.wap.multipart.alternative";
	  else if (octstr_case_compare(content_type, 
				      octstr_imm("multipart/mixed")) == 0)
	       s = "application/vnd.wap.multipart.mixed";
	  else if (DRM_CONTENT_TYPE(content_type) &&
		   mime_entity_num_parts(m) > 0) { /* fixup drm that might have been screwed up. */
	       Octstr *x = mime_entity_body(m);
	       while (mime_entity_num_parts(m) > 0)  /* remove them all. this message must not be parsed as mime. */
		    mime_entity_remove_part(m, 0); 
	       mime_entity_set_body(m, x);
	       octstr_destroy(x);
	  }
	  res = 1;
     }

     if (s)
	  strip_boundary_element(h,s);
     mime_replace_headers(m, h);
     http_destroy_headers(h);
     octstr_destroy(params);
     octstr_destroy(content_type);
     if ((n = mime_entity_num_parts(m))  > 0)
	  for (i = 0; i < n; i++) {
	       MIMEEntity *x = mime_entity_get_part(m, i);
	       int xres = unconvert_mime_msg(x);	     
	       if (xres) {		    
		    mime_entity_replace_part(m, i, x);  
		    res = 1;
	       }
	       mime_entity_destroy(x);
	  }
     return res;
}


MIMEEntity *mms_tomime(MmsMsg *msg, int base64)
{
     MIMEEntity *m;
     int i, n;
     
     if (!msg)
	  return NULL;

     m = mime_entity_create();
     mime_replace_headers(m, msg->headers);
     
     if (!msg->ismultipart)
	  mime_entity_set_body(m, msg->body.s);
     else {
	  for (i = 0, n = gwlist_len(msg->body.l); i < n; i++) {
	       MIMEEntity *mx = gwlist_get(msg->body.l, i);	       
	       mime_entity_add_part(m, mx);
	  }	  
     }
     convert_mime_msg(m);
     if (base64)
	  base64_mimeparts(m,0);
     return m;
}

static void fixup_date(List *headers, Octstr *hname)
{
     Octstr *s;
     if ((s = http_header_value(headers, hname)) != NULL) {
	  struct tm xtm, ytm, *tm;
	  time_t t = time(NULL), t2;
	  char buf[64], *p, *q;
	  
	  http_header_remove_all(headers, octstr_get_cstr(hname));
	  localtime_r(&t, &xtm); /* Initialise it. */
	  
	  strptime(octstr_get_cstr(s), "%a, %d %b %Y %T %z", &xtm); /* Parse date value with time zone. */

	  t2 = gw_mktime(&xtm); /* Convert to unix time... */
	  tm = gmtime_r(&t2, &ytm); /* Then convert to GM time. */
	  
	  if (!tm || asctime_r(tm, buf) == NULL) /* Then convert to ascii. If that fails...*/
	       ctime_r(&t, buf);                 /* .. just use current time. */
	  
	  /* Strip leading and trailing blanks. */
	  for (p = buf; *p && p < buf + sizeof buf; p++)
	    if (!isspace(*p))
	      break;
	  q = p + (strlen(p) - 1);
	  while (isspace(*q) && q > p)
	       *q-- = 0;
	  
	  http_header_add(headers, octstr_get_cstr(hname), p);
	  octstr_destroy(s);	         
     }
}

MmsMsg *mms_frommime(MIMEEntity *mime)
{
     MmsMsg *m;
     Octstr *s;
     MIMEEntity *mx;
     int i, n;
     List *h;
     
     if (!mime)
	  return NULL;

     m = gw_malloc(sizeof *m);
     memset(m, 0, sizeof *m);

     mx = mime_entity_duplicate(mime);

     unconvert_mime_msg(mx); /* Fix-up content type issues. */
     unpack_mimeheaders(mx);
     unbase64_mimeparts(mx);
     
     m->headers = mime_entity_headers(mx);     
     if ((n = mime_entity_num_parts(mx)) > 0) {
	  m->ismultipart  = 1;	  
	  m->body.l = gwlist_create();
	  for (i = 0; i < n; i++) 	       
	       gwlist_append(m->body.l, mime_entity_get_part(mx, i));	  
     } else {
	  m->ismultipart  = 0;	  
	  m->body.s = mime_entity_body(mx);
     } 
     mime_entity_destroy(mx); /* Because all its bits are used above. XXX not very clean! */

     
     /* Now check for important headers. If missing, put them in -  MsgId fixup - Vince */
     s = http_header_value(m->headers, octstr_imm("Message-ID"));
     if (s) {
	  octstr_replace(s, octstr_imm("<"), octstr_imm(""));
	  octstr_replace(s, octstr_imm(">"), octstr_imm(""));
	  if (octstr_get_char(s, 0) == '"') {
	       octstr_delete(s, 0, 1);
	       octstr_delete(s, octstr_len(s)-1, 1);
	  }
	  http_header_remove_all(m->headers, "Message-ID");
	  http_header_add(m->headers, "Message-ID", octstr_get_cstr(s));
	  m->msgId = octstr_duplicate(s);
	  octstr_destroy(s);
     }

     /* Default type is send */
     if ((s = http_header_value(m->headers, octstr_imm("X-Mms-Message-Type"))) == NULL ||
	 octstr_compare(s, octstr_imm("MM4_forward.REQ")) == 0) {
	  http_header_remove_all(m->headers, "X-Mms-Message-Type");
	  http_header_add(m->headers, "X-Mms-Message-Type", 
			  (char *)mms_message_type_to_cstr(MMS_MSGTYPE_SEND_REQ));
	  m->message_type = MMS_MSGTYPE_SEND_REQ;
	  if (s)
	       octstr_destroy(s);
     } else {
	  m->message_type = mms_string_to_message_type(s);
	  if (m->message_type < 0) {
	       mms_error(0, NULL, NULL, "Unknown message type: %s while parsing mime entity.", octstr_get_cstr(s));
	       octstr_destroy(s);
	       goto failed;
	  }
	  octstr_destroy(s);
     }

     if ((s = http_header_value(m->headers, octstr_imm("X-Mms-MMS-Version"))) == NULL)
	  http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     else
	  octstr_destroy(s);    	
     
     /* Fix-up date strings: Put it in GMT format, since it might not be. */
     fixup_date(m->headers, octstr_imm("Date"));
     fixup_date(m->headers, octstr_imm("X-Mms-Expiry"));
     fixup_date(m->headers, octstr_imm("X-Mms-Delivery-Time"));
     fixup_date(m->headers, octstr_imm("X-Mms-Previously-Sent-Date"));
     fixup_date(m->headers, octstr_imm("X-Mms-Reply-Charging-Deadline"));

     /* rebuild headers, skipping bad ones. */
     h = http_create_empty_headers();
     for (i = 0; i<gwlist_len(m->headers); i++) {
	  Octstr *name = NULL, *value = NULL;
	  http_header_get(m->headers, i, &name, &value);
	  if (mms_is_token(name))  /* if header name is bad, kill this header field. */
	       http_header_add(h, octstr_get_cstr(name), octstr_get_cstr(value));			       
	  octstr_destroy(name);
	  octstr_destroy(value);
     }
     http_destroy_headers(m->headers);
     m->headers = h;
     /* XXXX Probably ought to handle some more headers here: 
      * Return-Receipt-To becomes Read request is yes
      * Disposition-Notification-To: becomes X-Mms-Delivery-Report = Yes
      */

     /* XXXX Also need to validate this message a bit better. */
     fixup_msg(m, octstr_imm("anon@unknown"));

     return m;

 failed:
     mms_destroy(m);
     return NULL;
}


void mms_destroy(MmsMsg *msg)
{

     if (!msg)
	  return;
     if (msg->ismultipart) 
	  gwlist_destroy(msg->body.l, (gwlist_item_destructor_t *)mime_entity_destroy);     
     else if (msg->body.s)
	  octstr_destroy(msg->body.s);
     http_destroy_headers(msg->headers);
     if (msg->msgId) 
	  octstr_destroy(msg->msgId);
     gw_free(msg);
}

List *mms_message_headers(MmsMsg *msg)
{

     gw_assert(msg);
     return http_header_duplicate(msg->headers);
}

MmsMsg *mms_readreport(Octstr *msgid, Octstr *from, Octstr *to, time_t date, Octstr *status)
{

     MmsMsg *m;
     Octstr *s;

     
     m = gw_malloc(sizeof *m);
     m->ismultipart = 0;
     m->headers = http_create_empty_headers();

     m->message_type = MMS_MSGTYPE_READ_ORIG_IND;
     m->body.s = NULL;
     m->msgId = octstr_duplicate(msgid ? msgid : octstr_imm("none"));
     
     /* Now append headers. */
     
     http_header_add(m->headers, "X-Mms-Message-Type", "m-read-orig-ind");
     http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     
     http_header_add(m->headers, "Message-ID", msgid ? octstr_get_cstr(msgid) : "none");
     http_header_add(m->headers, "To", octstr_get_cstr(to));

     http_header_add(m->headers, "From", octstr_get_cstr(from));     

     s = date_format_http(date);
     http_header_add(m->headers, "Date", octstr_get_cstr(s));
     
     http_header_add(m->headers, "X-Mms-Status", octstr_get_cstr(status));
     
     octstr_destroy(s);

     return m;
}

MmsMsg *mms_deliveryreport(Octstr *msgid, Octstr *from, Octstr *to, time_t date, Octstr *status)
{

     MmsMsg *m = gw_malloc(sizeof *m);
     Octstr *s;

          
     m->ismultipart = 0;
     m->headers = http_create_empty_headers();

     m->message_type = MMS_MSGTYPE_DELIVERY_IND;
     m->body.s = NULL;
     m->msgId = octstr_duplicate(msgid ? msgid : octstr_imm("none"));
     
     /* Now append headers. */
     
     http_header_add(m->headers, "X-Mms-Message-Type", "m-delivery-ind");
     http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     
     http_header_add(m->headers, "Message-ID", msgid ? octstr_get_cstr(msgid) : "none");
     if (to)
	  http_header_add(m->headers, "To", octstr_get_cstr(to));
     if (from)
	  http_header_add(m->headers, "From", octstr_get_cstr(from));
     s = date_format_http(date);
     http_header_add(m->headers, "Date", octstr_get_cstr(s));
     
     http_header_add(m->headers, "X-Mms-Status", octstr_get_cstr(status));
     
     octstr_destroy(s);

     return m;
}

MmsMsg *mms_notification(Octstr *from, Octstr *subject, 
			 Octstr *mclass,
			 unsigned int msize, Octstr *url, 
			 Octstr *transactionid, time_t expiryt, int optimizesize)
{
     MmsMsg *m = gw_malloc(sizeof *m);
     char buf[10];

     time_t tnow = time(NULL);
     
     m->ismultipart = 0;
     m->msgId = NULL;
     m->body.s = NULL;
     m->headers = http_create_empty_headers();
     m->message_type = MMS_MSGTYPE_NOTIFICATION_IND;
     
     http_header_add(m->headers, "X-Mms-Message-Type", "m-notification-ind");
     http_header_add(m->headers, "X-Mms-Transaction-ID", 
		     octstr_get_cstr(transactionid));
     http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     
     if (from)
	  http_header_add(m->headers, "From", octstr_get_cstr(from));

     if (subject && !optimizesize)
	  http_header_add(m->headers, "Subject", octstr_get_cstr(subject));
     
     http_header_add(m->headers, "X-Mms-Message-Class", 
		     mclass ? octstr_get_cstr(mclass) : "Personal");
     
     sprintf(buf, "%d", msize);
     http_header_add(m->headers, "X-Mms-Message-Size", buf);
     
#define LARGET 365*24*3600

     sprintf(buf, "%ld", expiryt ? expiryt - tnow : LARGET);
     http_header_add(m->headers, "X-Mms-Expiry", buf);


    /* No reply charge stuff for now. */     
    
     http_header_add(m->headers, "X-Mms-Content-Location", octstr_get_cstr(url));
     
     return m;
}

MmsMsg *mms_retrieveconf(MmsMsg *msg, Octstr *transactionid, 
			 char *err, char *errtxt, Octstr *opt_from, 
			 int menc)
{
     MmsMsg *m;

     m  = gw_malloc(sizeof *m);
     m->msgId = msg ? octstr_duplicate(msg->msgId) : octstr_imm("00000");

     m->headers = http_create_empty_headers();
     m->message_type = MMS_MSGTYPE_RETRIEVE_CONF;
     
     http_header_add(m->headers, "X-Mms-Message-Type", "m-retrieve-conf");
     if (transactionid)
	  http_header_add(m->headers, "X-Mms-Transaction-ID", 
			  octstr_get_cstr(transactionid));

     if (menc >= MS_1_2)
	  http_header_add(m->headers, "X-Mms-MMS-Version", "1.2");
     else
	  http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);

     if (!msg) {	  
	  Octstr *x = date_format_http(time(NULL));

	  m->ismultipart = 0;
	  
	  http_header_add(m->headers, "Date", octstr_get_cstr(x));
	  http_header_add(m->headers, "X-Mms-Retrieve-Status",err);
	  if (err) 
	       http_header_add(m->headers, "X-Mms-Retrieve-Text",err);
	  if (opt_from)
	       http_header_add(m->headers, "From", octstr_get_cstr(opt_from));
	  http_header_add(m->headers, "Content-Type", "text/plain");
	  
	  if (errtxt)
	       m->body.s = octstr_create(errtxt);
	  else
	       m->body.s = octstr_create(" ");

	  octstr_destroy(x);	  
     } else { /* Otherwise copy from message. */
	  List *h = mms_message_headers(msg);
	  int i, n;

#if 0	  /* Some phones do not like this header! */
	  http_header_add(m->headers, "X-Mms-Retrieve-Status", "Ok");
#endif

	  http_header_combine(h, m->headers);	  
	  http_destroy_headers(m->headers);
	  m->headers = h;
	  
	  m->ismultipart = msg->ismultipart;

	  if (!m->ismultipart)
	       m->body.s = octstr_duplicate(msg->body.s);
	  else 
	       /* Body is a list of MIMEEntities, so recreate it. */
	       
	       for (m->body.l = gwlist_create(), i = 0, 
			 n = gwlist_len(msg->body.l); 
		    i<n; i++) 
		    gwlist_append(m->body.l, 
				mime_entity_duplicate(gwlist_get(msg->body.l, i)));
	  /* Remove some headers that may not be permitted. */
	  mms_remove_headers(m, "X-Mms-Expiry");
	  mms_remove_headers(m, "X-Mms-Delivery-Time");
	  mms_remove_headers(m, "X-Mms-Sender-Visibility");
     }
     return m;
}

int mms_remove_headers(MmsMsg *m, char *name)
{
     gw_assert(m);
     http_header_remove_all(m->headers, name);
     return 0;
}

MmsMsg *mms_sendconf(char *errstr, char *msgid, char *transid, int isforward, int menc)
{
     MmsMsg *m = gw_malloc(sizeof *m);

     
     m->ismultipart = 0;
     m->msgId = msgid ? octstr_create(msgid) : NULL;
     m->body.s = NULL;

     m->headers = http_create_empty_headers();

     if (!isforward) {
	  m->message_type = MMS_MSGTYPE_SEND_CONF;	  
	  http_header_add(m->headers, "X-Mms-Message-Type", "m-send-conf");
     } else {
	  m->message_type = MMS_MSGTYPE_FORWARD_CONF;	  
	  http_header_add(m->headers, "X-Mms-Message-Type", "m-forward-conf");
     }
     http_header_add(m->headers, "X-Mms-Transaction-ID", transid);
     
     if (menc >= MS_1_2)
	  http_header_add(m->headers, "X-Mms-MMS-Version", "1.2");
     else
	  http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     
     http_header_add(m->headers, "X-Mms-Response-Status", errstr);
     
     if (msgid)
	  http_header_add(m->headers, "Message-ID", msgid);

     return m;
}

MmsMsg *mms_notifyresp_ind(char *transid, int menc, char *status, int report_allowed)
{
     MmsMsg *m = gw_malloc(sizeof *m);

     
     m->ismultipart = 0;
     m->msgId =  NULL;
     m->body.s = NULL;

     m->headers = http_create_empty_headers();

     m->message_type = MMS_MSGTYPE_NOTIFYRESP;	  
     http_header_add(m->headers, "X-Mms-Message-Type", "m-notifyresp-ind");
     http_header_add(m->headers, "X-Mms-Transaction-ID", transid);
     
     if (menc >= MS_1_2)
	  http_header_add(m->headers, "X-Mms-MMS-Version", "1.2");
     else
	  http_header_add(m->headers, "X-Mms-MMS-Version", MMS_DEFAULT_VERSION);
     
     
     http_header_add(m->headers, "X-Mms-Status",  status);     
     http_header_add(m->headers, "X-Mms-Report-Allowed",  report_allowed ? "Yes" : "No");
     
     return m;
}

int mms_make_sendreq(MmsMsg *retrieveconf)
{

     gw_assert(retrieveconf);

     if (retrieveconf->message_type == MMS_MSGTYPE_SEND_REQ)
	  return 0;

     gw_assert(retrieveconf->message_type == MMS_MSGTYPE_RETRIEVE_CONF);
     
     retrieveconf->message_type = MMS_MSGTYPE_SEND_REQ;     
     mms_replace_header_value(retrieveconf, "X-Mms-Message-Type", 
			      (char *)mms_message_type_to_cstr(MMS_MSGTYPE_SEND_REQ));
     
     return 0;
}

int mms_replace_header_value(MmsMsg *msg, char *hname, char *value)
{
     gw_assert(msg);
     http_header_remove_all(msg->headers, hname);
     http_header_add(msg->headers, hname, value);
     return 0;
}

int mms_add_missing_headers(MmsMsg *msg, List *headers)
{
     List *h;
     
     gw_assert(msg);
     h = http_header_duplicate(headers);
     http_header_combine(h, msg->headers);
     
     http_destroy_headers(msg->headers);
     msg->headers = h;
     return 0;
}

int mms_replace_header_values(MmsMsg *msg, char *hname, List *value)
{
     int i;

     gw_assert(msg);
     http_header_remove_all(msg->headers, hname);

     for (i = 0; i < gwlist_len(value); i++) {
	  Octstr *x = gwlist_get(value, i);
	  http_header_add(msg->headers, hname, octstr_get_cstr(x));
     }
     return 0;
}

Octstr *mms_get_header_value(MmsMsg *msg, Octstr  *header)
{
     gw_assert(msg);
     return http_header_value(msg->headers, header);     
}


List *mms_get_header_values(MmsMsg *msg, Octstr  *header)
{    
     List *h; 
     List *l;
     int i;
    
    gw_assert(msg);    
    l = gwlist_create();    
    h = http_header_find_all(msg->headers, octstr_get_cstr(header));
    for (i = 0; i < gwlist_len(h); i++) {
	 Octstr *hname, *value;
	 
	 http_header_get(h, i, &hname, &value);

	 gwlist_append(l, value);
	 octstr_destroy(hname);
    }
    http_destroy_headers(h);
    return l;
}


int mms_convert_readrec2readorig(MmsMsg *msg)
{
     Octstr *s;
     
     gw_assert(msg);
     if (msg->message_type != MMS_MSGTYPE_READ_REC_IND)
	  return -1;
     
     mms_replace_header_value(msg, "X-Mms-Message-Type", "m-read-orig-ind");
     msg->message_type = MMS_MSGTYPE_READ_ORIG_IND;
     
     if ((s = mms_get_header_value(msg, octstr_imm("Date"))) == NULL) {
	  time_t t  = time(NULL);
	  s = date_format_http(t);
	  mms_replace_header_value(msg, "Date", octstr_get_cstr(s));
     }      
     octstr_destroy(s);
	  
     return 0;
}

MmsMsg *mms_storeconf(char *errstr, char *transid, Octstr *msgloc, int isupload, int menc)
{
     MmsMsg *m = gw_malloc(sizeof *m);
     
     
     m->ismultipart = 0;
     m->msgId = NULL;
     m->body.s = NULL;

     m->headers = http_create_empty_headers();

     if (!isupload) {
	  m->message_type = MMS_MSGTYPE_MBOX_STORE_CONF;	  
	  http_header_add(m->headers, "X-Mms-Message-Type", "m-mbox-store-conf");
     } else {
	  m->message_type = MMS_MSGTYPE_MBOX_UPLOAD_CONF;	  
	  http_header_add(m->headers, "X-Mms-Message-Type", "m-mbox-upload-conf");
     }
     http_header_add(m->headers, "X-Mms-Transaction-ID", transid);     
     http_header_add(m->headers, "X-Mms-MMS-Version", "1.2");  /* ignore menc for now. */     
     http_header_add(m->headers, "X-Mms-Store-Status", errstr);
     
     if (msgloc)
	  http_header_add(m->headers, "X-Mms-Content-Location", octstr_get_cstr(msgloc));

     return m;
}

MmsMsg *mms_deleteconf(int menc, char *transid)
{
     MmsMsg *m = gw_malloc(sizeof *m);
     
     
     m->ismultipart = 0;
     m->msgId = NULL;
     m->body.s = NULL;

     m->headers = http_create_empty_headers();
     m->message_type = MMS_MSGTYPE_MBOX_DELETE_CONF;	  
     http_header_add(m->headers, "X-Mms-Message-Type", "m-mbox-delete-conf");
	  
     http_header_add(m->headers, "X-Mms-Transaction-ID", transid);     
     http_header_add(m->headers, "X-Mms-MMS-Version", "1.2");  /* ignore menc for now. */
     
     return m;
}

static int mms_msgsize(MmsMsg *m)
{
     Octstr *s;
     int n;
     
     gw_assert(m);
     s = mms_tobinary(m); /* Dirty, but works... */
     n = octstr_len(s);
     octstr_destroy(s);
     return n;
}

static int mms_convert_to_mboxdescr(MmsMsg *mm, Octstr *cloc, List *reqhdrs,
				    unsigned long msize)
{
     int i, n;
     List *mh, *xh;
     Octstr *xstate;
     int addcontent = 0, hasmsgid = 0;

     if (!mm)
	  return -1;
     
     mh = http_create_empty_headers();
     
     mm->message_type = MMS_MSGTYPE_MBOX_DESCR;
     
     http_header_add(mh, "X-Mms-Message-Type", "m-mbox-descr");
     http_header_add(mh, "X-Mms-MMS-Version", "1.2");
     http_header_add(mh, "X-Mms-Content-Location", octstr_get_cstr(cloc));

     /* Add only those headers requested. */
     for (i = 0, n = gwlist_len(reqhdrs); i < n; i++) { 
	  Octstr *header = gwlist_get(reqhdrs,i);
	  List *h = http_header_find_all(mm->headers, octstr_get_cstr(header));
	  int j;

	  if (octstr_case_compare(header, octstr_imm("Content")) == 0) {
	       addcontent = 1;
	       goto loop;
	  } else if (octstr_case_compare(header, octstr_imm("X-Mms-Message-Size")) == 0) {
	       char yy[64];
	       
	       sprintf(yy, "%lu", msize);
	       http_header_add(mh, "X-Mms-Message-Size", yy);
	       goto loop;
	  } else if (octstr_case_compare(header, octstr_imm("Message-ID")) == 0) 
	       hasmsgid = 1;
	  
	  for (j = 0; j < gwlist_len(h); j++) {
	       Octstr *hname, *value;	       
	       http_header_get(h, j, &hname, &value);
	       octstr_destroy(hname);
	       
	       http_header_add(mh, octstr_get_cstr(header), octstr_get_cstr(value)); 
	       
	       octstr_destroy(value);
	  }
	  
     loop:
	  http_destroy_headers(h);
	  /* We ignore the extra attributes request. */
     }

     if (!hasmsgid) {
	  Octstr *v = http_header_value(mm->headers, octstr_imm("Message-ID"));
	  if (v) {
	       http_header_add(mh, "Message-ID", 
			       octstr_get_cstr(v));
	       octstr_destroy(v);
	  }	  
     }

     /* Copy over the MM-State and MM-flags headers. */
     xh = http_header_find_all(mm->headers, "X-Mms-MM-Flags");
     if (xh) {
	  http_header_combine(mh,xh);
	  http_destroy_headers(xh);
     }
     xstate = http_header_value(mm->headers, octstr_imm("X-Mms-MM-State"));
     if (xstate) {
	  http_header_remove_all(mh, "X-Mms-MM-State");
	  http_header_add(mh, "X-Mms-MM-State", octstr_get_cstr(xstate));
	  octstr_destroy(xstate);
     }

     if (!addcontent) {
	  if (mm->ismultipart) 
	       gwlist_destroy(mm->body.l, (gwlist_item_destructor_t *)mime_entity_destroy);   
	  else if (mm->body.s)
	       octstr_destroy(mm->body.s);
	  mm->body.s = NULL;
     } else {
	  /* copy over content type. */
	  Octstr *ctype = http_header_value(mm->headers, octstr_imm("Content-Type"));
	  if (ctype) {
	       http_header_add(mh, "Content-Type", octstr_get_cstr(ctype));
	       octstr_destroy(ctype);
	  }
     }

     http_destroy_headers(mm->headers);
     mm->headers = mh;
     return 0;
}

MmsMsg *mms_viewconf(char *transid, 
		     List *msgrefs, 
		     List *msglocs,
		     char *err,
		     List *required_headers,
		     MmsMsgGetFunc_t *getmsg, 
		     void *p1, void *p2,		     
		     int maxsize, int menc,
		     List *otherhdrs)
{     
     MmsMsg *m = gw_malloc(sizeof *m);
     int msize, i, n;
     int msgcount;

     err = err ? err : "Ok";

     m->ismultipart = 0;
     m->msgId = NULL;
     m->body.s = NULL;

     m->headers = http_create_empty_headers();
     m->message_type = MMS_MSGTYPE_MBOX_VIEW_CONF;	  
     http_header_add(m->headers, "X-Mms-Message-Type", "m-mbox-view-conf");
	  
     http_header_add(m->headers, "X-Mms-Transaction-ID", transid);     
     http_header_add(m->headers, "X-Mms-MMS-Version", "1.2");  /* ignore menc for now. */

     /* Put in some dummy  headers so count works fine ... */

     http_header_add(m->headers, "X-Mms-Message-Count", "0");     
     http_header_add(m->headers, "Content-Type", "*/*"); /* we'll change this later. */

     http_header_combine(m->headers, otherhdrs); /* add any other hdrs passed. */

     if (msgrefs == NULL || 
	 strcasecmp(err, "ok") != 0) { /* We got an error. */
	  http_header_add(m->headers, "X-Mms-Response-Status",
			  err ? err : "Error-unspecified");
	  return m;
     } else	  
	  http_header_add(m->headers, "X-Mms-Response-Status", "Ok");

     msize = mms_msgsize(m);
     n = gwlist_len(msgrefs);
     msgcount = 0;
     
     m->ismultipart = 1;
     m->body.l = gwlist_create();
     for (i = 0; i < n; i++) {
	  unsigned long tmsize;
	  Octstr *msgref = gwlist_get(msgrefs,i);
	  Octstr *msgloc = gwlist_get(msglocs, i);
	  MmsMsg *mm = getmsg(p1, p2, msgref, &tmsize); 
	  Octstr *ms;
	  
	  if (mms_convert_to_mboxdescr(mm,
				       msgloc,
				       required_headers, tmsize) != 0)
	       goto loop;

	  ms =  mms_tobinary(mm);
	  if (octstr_len(ms)  + msize <= maxsize) {
	       MIMEEntity *mtmp = mime_entity_create();
	       List *h = mime_entity_headers(mtmp);
	       
	       http_header_add(h, "Content-Type", 
			       "application/vnd.wap.mms-message");
	       mime_replace_headers(mtmp, h);
	       mime_entity_set_body(mtmp, ms);

	       http_destroy_headers(h);
	       
	       gwlist_append(m->body.l, mtmp);	       
	       msgcount++;
	       msize += octstr_len(ms);
	  } else {
	       i = n; /* force end. */
	       octstr_destroy(ms);
	  }
	  
	  mms_destroy(mm);
	  
     loop:(void)0;
     }
     
     if (gwlist_len(m->body.l) > 0) {
	  char x[32];
	  sprintf(x, "%d", (int)gwlist_len(m->body.l));
	  mms_replace_header_value(m, "X-Mms-Message-Count", x);
	  mms_replace_header_value(m, "Content-Type", "application/vnd.wap.multipart.mixed");
     } else {
	  gwlist_destroy(m->body.l,NULL);
	  m->body.s = NULL;
	  m->ismultipart = 0;
     }

     return m;
}

void *mms_msgbody(MmsMsg *msg)
{
     if (!msg)
	  return NULL;

     if (msg->ismultipart) {
	  List *l = gwlist_create();
	  int i;
	  
	  for (i = 0; i < gwlist_len(msg->body.l); i++) 
	       gwlist_append(l, mime_entity_duplicate(gwlist_get(msg->body.l,i)));
	  return l;
     } else
	  return octstr_duplicate(msg->body.s);     
}

int mms_clearbody(MmsMsg *msg)
{
     if (!msg)
	  return -1;

     if (msg->ismultipart) 
	  gwlist_destroy(msg->body.l, (gwlist_item_destructor_t *)mime_entity_destroy);     
     else if (msg->body.s)
	  octstr_destroy(msg->body.s);
     msg->body.s = NULL;
     msg->ismultipart = 0;
     http_header_remove_all(msg->headers, "Content-Type");     

     return 0;
}

int mms_putbody(MmsMsg *msg, void *body, int ismultipart)
{
     gw_assert(msg);
     mms_clearbody(msg);

     msg->ismultipart = ismultipart;
     msg->body.s = body;

     return 0;
}

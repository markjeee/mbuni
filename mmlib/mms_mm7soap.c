/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MM7/SOAP message encoder/decoder and helper functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */

#include "mms_mm7soap.h"

/* function to traverse SOAP env:Body extracting useful headers. */

#define dfltstr(e) ((e) ? ((char *)(e)) : "")

#define content(n) ((n)->xmlChildrenNode ? dfltstr(((n)->xmlChildrenNode)->content) : dfltstr((n)->content))

struct MSoapMsg_t {
     List *envelope; /* of http headers. */
     MIMEEntity *msg; /* XXX - be sure to copy some headers to this from envelope before convert...*/
};

/* We expect ISO formatted time, or interval. */

static time_t parse_time(char *s)
{
     time_t t = time(NULL);
     Octstr *p = octstr_create(s);
     int i = 0, secs = 0;

     octstr_strip_blanks(p);

     if (s && s[0] != 'P') {
	  struct universaltime tt;
	  if (date_parse_iso(&tt, p) >= 0)
	       t = date_convert_universal(&tt);
	  goto done;
     } else 
	  i++;

     while (i < octstr_len(p)) {
	  long n = 0;
	  int ch;
	  
	  if (octstr_get_char(p, i) == 'T') {
	       secs = 1;
	       i++;
	  }
	  
	  i = octstr_parse_long(&n, p, i, 10);
	  
	  if (i < 0)
	       break;
	  ch = octstr_get_char(p, i);
	  i++;
	  switch(ch) {
	  case 'Y': /* years. approx to 365 1/4 days. */
	       t += (365.25*24*3600*n);
	       break;
	  case 'M': /* month or minutes. approx month = 30 days. */
	       if (secs)
		    t += n*60;
	       else 
		    t += 30*24*3600*n; 
	       break;
	  case 'D':
	       t += n*24*3600;
	       break;
	  case 'H': /* hours. */
	       t += n*3600;
	       break;
	  case 'S':
	       t += n;
	       break;
	  default:
	       break;
	  }
     }     

 done:
     octstr_destroy(p);
     return t;
}

static int parse_header(xmlNodePtr node, List *headers, int *sigparent)
{
     int skip = 0;
     int tag;
     char *hname;
     unsigned char *s = NULL;
     char *nvalue;
     Octstr *value = NULL, *tmp;
     
     
     if (!node || node->type != XML_ELEMENT_NODE)
	  return -1;

     /* look at each node in turn, extract meaning.
      * we ignore some tags: senderidentification, etc because we don't need them.
      * we are also not strict on syntax (when receiving): we will be on sending!
      */
     
     hname = (char *)node->name;
     nvalue = content(node);

     tmp = octstr_create(hname);
     tag = mms_string_to_mm7tag(tmp);
     octstr_destroy(tmp);

     switch(tag) {
     case MM7_TAG_CancelReq:
     case MM7_TAG_CancelRsp:
     case MM7_TAG_DeliverReq:
     case MM7_TAG_DeliverRsp:
     case MM7_TAG_DeliveryReportReq:
     case MM7_TAG_DeliveryReportRsp:
     case MM7_TAG_RSErrorRsp:
     case MM7_TAG_ReadReplyReq:
     case MM7_TAG_ReadReplyRsp:
     case MM7_TAG_ReplaceReq:
     case MM7_TAG_ReplaceRsp:
     case MM7_TAG_SubmitReq:
     case MM7_TAG_SubmitRsp:
     case MM7_TAG_VASPErrorRsp:
	  hname = "MessageType";
	  value = mms_mm7tag_to_string(tag);
	  break;
     case MM7_TAG_SenderIdentification:
     case MM7_TAG_Recipients:

       skip = 1;
       break;

     case MM7_TAG_Recipient:
	  *sigparent = MM7_TAG_To; /* make it a To field. */
	  break;
     case MM7_TAG_To:
     case MM7_TAG_Cc:
     case MM7_TAG_Bcc:
     case MM7_TAG_Sender:	
     case MM7_TAG_SenderAddress:	  
	  if (has_node_children(node)) { /* account for buggy senders! XXX */      
	       skip = 1;
	       *sigparent = tag; /* wait for number. */
	  } else {
	       char *sx;
	       char *s = (void *)xmlGetProp(node, (unsigned char *)"displayOnly");
	       if (s && strcasecmp((char *)s, "true") == 0) /* a '-' indicates don't use this to send. */
		    sx = "-";
	       else 
		    sx = "+";
	       value = octstr_format("%s %s", sx, nvalue);
	       if (s) xmlFree(s);
	  }
	  break;
     case MM7_TAG_Content: 	  
	  if ((s = xmlGetProp(node, (unsigned char *)"href")) != NULL) {
	       value = octstr_create((char *)s); 
	       xmlFree(s);
	  }
	  
	  if ((s = xmlGetProp(node, (unsigned char *)"allowAdaptations")) != NULL) {
	       http_header_add(headers, "allowAdaptations", (char *)s);
	       xmlFree(s);
	  }
	  break;
     case MM7_TAG_ShortCode:
     case MM7_TAG_Number: /* we will not normalise number here, that's for upper level. */
	  value = octstr_format("%s/TYPE=PLMN", nvalue);

	  /* -- fall through. -- */
     case MM7_TAG_RFC2822Address:
	  if (!value)
	       value = octstr_create(nvalue);
	  hname = (char *)mms_mm7tag_to_cstr(*sigparent); /* real tag is parent. */
	  
	  s = xmlGetProp(node, (unsigned char *)"displayOnly");
	  if (s && strcasecmp((char *)s, "true") == 0) /* a '-' indicates don't use this to send. */
	       octstr_insert(value, octstr_imm("- "), 0);
	  else 
	       octstr_insert(value, octstr_imm("+ "), 0);
	  if (s)
	       xmlFree(s);
 	  break;

     case MM7_TAG_EarliestDeliveryTime:
     case MM7_TAG_ExpiryDate:
     case MM7_TAG_TimeStamp:
     case MM7_TAG_Date:
	  value = date_format_http(parse_time(nvalue));
	  break;
	  
     case MM7_TAG_ReplyCharging:
	  value = octstr_create("Requested");
	  
	  if ((s = xmlGetProp(node, (unsigned char *)"replyChargingSize")) != NULL) {
	       http_header_add(headers, "replyChargingSize", (char *)s);
	       xmlFree(s);
	  }
	  if ((s = xmlGetProp(node, (unsigned char *)"replyDeadline")) != NULL) {
	       Octstr *t = date_format_http(parse_time((char *)s));	       
	       http_header_add(headers, "replyDeadline", octstr_get_cstr(t));
	       xmlFree(s);
	       octstr_destroy(t);
	  }
	  break;	  
     case MM7_TAG_UACapabilities:
	  if ((s = xmlGetProp(node, (unsigned char *)"UAProf")) != NULL) {	  
	       value = octstr_create((void *)s);
	       xmlFree(s);
	  } else 
	       value = octstr_create("none");
	  
	  if ((s = xmlGetProp(node, (unsigned char *)"TimeStamp")) != NULL) {	  
	       time_t lt = parse_time((char *)s);
	       octstr_format_append(value, ",%ld", lt);
	       xmlFree(s);
	  }
	  break;
     default:
	  break;
     }
     
     if (!value) 
	  value = octstr_create(nvalue);
     octstr_strip_blanks(value);

     if (!skip && tag >= 0 && hname != NULL) {
	  http_header_add(headers, hname, octstr_get_cstr(value));     
#if 1
	  mms_info(0, "parse.soap", NULL, "h=%s, v=%s!", hname, octstr_get_cstr(value));
#endif 
     }
     octstr_destroy(value);

     return 0;
}

static int parse_headers(xmlNodePtr start, List *h, int sigparent)
{
     xmlNodePtr x;

     for (x = start; x; x = x->next) 
	  if (x->type != XML_COMMENT_NODE) {
	       parse_header(x, h, &sigparent);	       
	       parse_headers(x->xmlChildrenNode, h, sigparent);
	  }
     return 0;
}

MSoapMsg_t *mm7_parse_soap(List *headers, Octstr *body)
{
     MIMEEntity  *mime = mime_http_to_entity(headers, body), *start = NULL;
     Octstr *xml = NULL, *cloc;
     xmlDocPtr doc;
     MIMEEntity *msg = NULL;
     List *h;
     int s = -1;     
     MSoapMsg_t *smsg = NULL;
     
     if (!mime) 
	  return NULL;
     /* Find the start element: 
      * - either the mime entity is multipart and has start param (or implicitly first element) ...
      * - or entity is not multipart, so body is xml
      */
          
     if ((start = mime_multipart_start_elem(mime)) != NULL) 
	  xml = mime_entity_body(start);
     else
	  xml = mime_entity_body(mime);
     
     if (!xml) 
	  goto done;
     octstr_strip_blanks(xml);
#if  1
     debug("mms2soap", 0, "XML sent is: %s!", octstr_get_cstr(xml));
#endif
     doc = xmlParseMemory(octstr_get_cstr(xml), octstr_len(xml));
     if (!doc || !doc->xmlChildrenNode)
	  goto done;
     
     h = http_create_empty_headers();

     parse_headers(doc->xmlChildrenNode, h, s);
     xmlFreeDoc(doc);
     
     if (!h)
	  goto done;
     
     cloc = http_header_value(h, octstr_imm("Content"));
     
     if (cloc) {
	  /* XXXX only support content that is inline.  easy to add external. */
	  MIMEEntity *c = NULL;
	  int i, n;
	  char *loc = octstr_get_cstr(cloc);
	  
	  if (octstr_case_search(cloc, octstr_imm("cid:"), 0) == 0) /* skip 'cid:' part. */
	       loc += 4; /* XXX This should always be there, but some MMC don't put it there. */
	  
	  for (i = 0, n = mime_entity_num_parts(mime); i<n; i++) {
	       MIMEEntity *x = mime_entity_get_part(mime, i);
	       List *headers = mime_entity_headers(x); 
	       Octstr *y = headers ? http_header_value(headers, octstr_imm("Content-ID")) : NULL;
	       char *cid = (y && octstr_get_char(y, 0) == '<') ? octstr_get_cstr(y) + 1 : (y ? octstr_get_cstr(y) : "");
	       int cid_len = (y && octstr_get_char(y, 0) == '<') ? octstr_len(y) - 2 : (y ? octstr_len(y) : 0);
	       
	       if (y && 
		   (strncmp(loc, cid, cid_len) == 0 ||
		    octstr_compare(y, cloc) == 0)) /* XXX seems wrong, but some MMC out there behave badly, so we support it */
		    c = mime_entity_duplicate(x);
	       
	       octstr_destroy(y);
	       http_destroy_headers(headers);
	       mime_entity_destroy(x);
	       if (c) 
		    break;
	  } 
	  if (c)
	       msg = c; /* this is already a copy. */	  
	  octstr_destroy(cloc);	  
     } else {
	       /* XXX Sigh! Broken MMC don't set this parameter. Lets try to be kind to them! */
	       int i, n = mime_entity_num_parts(mime);
	       int found = 0;
	       for (i = 0; !found && i < n; i++) {
		    MIMEEntity *x = mime_entity_get_part(mime,i);
		    List *hx = NULL;
		    Octstr *xctype = NULL, *xparams = NULL;
		    if (!x)
			 goto loop;
		    hx = mime_entity_headers(x);
		    get_content_type(hx, &xctype, &xparams);
		    
		    if (xctype && octstr_str_case_compare(xctype, "text/xml") != 0) {
			 /* Found something that might be the body, since its not XML. 
			  * Perhaps we compare bodies? For broken ones anyway, so no bother!
			  */
			 found = 1;
			 msg = x;
		    }
	       loop:
		    if (x && !found) mime_entity_destroy(x);
		    if (hx) http_destroy_headers(hx);
		    octstr_destroy(xctype);
		    octstr_destroy(xparams);
	       }
     }
          
     smsg = gw_malloc(sizeof *smsg);
     smsg->envelope = h;
     smsg->msg = msg;

 done:
     if (mime)
	  mime_entity_destroy(mime);
     if (start)
	  mime_entity_destroy(start);

     octstr_destroy(xml);
     return smsg;
}

static int append_address(Octstr *p, Octstr *addr_spec, char *prefix, int add_type)
{
     Octstr *v = addr_spec, *z;
     char *y, *typ = "";
     int j, ch = octstr_get_char(v, 0);
     if (ch == '-')
	  y = " displayOnly=\"true\"";
     else 
	  y = "";
     j = octstr_case_search(v, octstr_imm("/TYPE=PLMN"),0);
     if (j >= 0 && octstr_search_char(v, '@', 0) < 0) { /* NO '@' */
	  z = octstr_copy(v, 2, j-2); /* skip the initial char that is only for info purposes. */
	  typ = octstr_len(z) <= 6 ? "ShortCode" : "Number";
     } else {
	  z  = octstr_copy(v, 2, octstr_len(v));
	  typ = "RFC2822Address";
     }

     octstr_strip_blanks(z);
     if (add_type)
	  octstr_format_append(p, "<%s%s%s>%S</%s%s>\n", 
			       prefix, typ, y, z, prefix, typ); /* as above... */
     else 
	  octstr_append(p, z);

     octstr_destroy(z);
     return 0;
}

static void output_rcpt(char *hdr, List *hdrs, Octstr *p, char *prefix)
{
     List *l = http_header_find_all(hdrs, hdr);
     char x[32];
     int i, n;

     for (i = 0, n = gwlist_len(l), x[0]=0; i < n; i++) {
	  Octstr *h = NULL, *v  = NULL;
	  http_header_get(l, i, &h, &v);

	  if (octstr_str_compare(h, x) != 0) {
	       if (x[0])
		    octstr_format_append(p, "</%s%s>\n", prefix, x);
	       strncpy(x, octstr_get_cstr(h), sizeof x);
	       octstr_format_append(p, "<%s%S>\n", prefix, h);
	  }
	  octstr_destroy(h);
	  append_address(p, v, prefix,1); /* put the address in */
	  octstr_destroy(v);
     }
     if (x[0]) /* close it off. */
	  octstr_format_append(p, "</%s%s>\n", prefix, x);    

     http_destroy_headers(l);
}

/* compare two mm7 version xmlns strings. Crude, but should generally work.  */
static int ver_compare(MM7Version_t *v1, MM7Version_t *v2)
{
     return (v1->major-v2->major)*1000 + (v1->minor1-v2->minor1)*100 + (v1->minor2-v2->minor2);     
}


#define XMLNSMM7 "http://www.3gpp.org/ftp/Specs/archive/23_series/23.140/schema/REL-5-MM7-1-0"
#define SOAP_ENV "SOAP-ENV"

/* Construct by hand. */
static Octstr *headers_to_soapxml(List *hdrs, MM7Version_t *ver)
{
     Octstr *s = octstr_create("<?xml version='1.0' encoding='UTF-8'?>\n");
     Octstr *xmlns =  octstr_create((ver->xmlns[0]) ? ver->xmlns : XMLNSMM7);     
     Octstr *p, *q, *r, *fault, *mtype;
     int i, n, mtag;
     time_t t;
     char *prefix = ver->use_mm7_namespace ? "mm7:" : "";
     char *xml_nsp = ver->use_mm7_namespace ? ":mm7" : "";
     MM7Version_t min_ver = {5,1,0}; /* minimum version we deal with. */
     
     octstr_append_cstr(s, 
		   "<" SOAP_ENV ":Envelope xmlns:" SOAP_ENV "=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
		   "<" SOAP_ENV ":Header>\n");
     p = http_header_value(hdrs, octstr_imm("TransactionID"));
     
     octstr_format_append(s, "<%sTransactionID xmlns%s=\"%s\" " SOAP_ENV 
			  ":mustUnderstand=\"1\">%S</%sTransactionID>\n",
			  prefix, xml_nsp,
			  octstr_get_cstr(xmlns), p ? p : octstr_imm("none"),
			  prefix);
     if (p)
	  octstr_destroy(p);

     octstr_append_cstr(s, "</" SOAP_ENV ":Header>\n<" SOAP_ENV ":Body>\n");

     fault = http_header_value(hdrs, octstr_imm("Fault"));     
     
     if (fault) {
	  Octstr *fc = http_header_value(hdrs, octstr_imm("faultcode"));     
	  Octstr *fs = http_header_value(hdrs, octstr_imm("faultstring"));      
	  
	  octstr_append_cstr(s, "<" SOAP_ENV ":Fault>\n");
	  if (fc) {
	       octstr_format_append(s, "<faultcode>%S</faultcode>\n", fc);
	       octstr_destroy(fc);
	  }
	  if (fs) {
	       octstr_format_append(s, "<faultstring>%S</faultstring>\n", fs);
	       octstr_destroy(fs);
	  }
	  octstr_append_cstr(s, "<detail>\n");
     }

     
     mtype = http_header_value(hdrs, octstr_imm("MessageType"));     
     mtag = mms_string_to_mm7tag(mtype);

     octstr_format_append(s, "<%s%S xmlns%s=\"%s\">\n", prefix, mtype, xml_nsp, octstr_get_cstr(xmlns));
     
     /* Output the details. */
     if ((p = http_header_value(hdrs, octstr_imm("MM7Version"))) != NULL) {
	  octstr_format_append(s, "<%sMM7Version>%S</%sMM7Version>\n", prefix, p, prefix);
	  octstr_destroy(p);
     } else 
	  octstr_format_append(s, "<%sMM7Version>%d.%d.%d</%sMM7Version>\n", 
			       prefix, ver->major, ver->minor1, ver->minor2, prefix);
     
     p = http_header_value(hdrs, octstr_imm("VASPID"));     
     q = http_header_value(hdrs, octstr_imm("VASID"));     
     r = http_header_value(hdrs, octstr_imm("SenderAddress"));     
     if (p || q || r) {

	  octstr_format_append(s, "<%sSenderIdentification>\n", prefix);
	  if (p) 
	       octstr_format_append(s, "<%sVASPID>%S</%sVASPID>\n", prefix, p, prefix);

	  if (q) 
	       octstr_format_append(s, "<%sVASID>%S</%sVASID>\n", prefix, q, prefix);

	  if (r) {
	       Octstr *xx = octstr_create("");
	       append_address(xx, r, prefix, 
			      ver_compare(ver,&min_ver) > 0);	  	       
	       if (octstr_len(xx) > 0)
		    octstr_format_append(s, "<%sSenderAddress>%S</%sSenderAddress>\n", prefix, xx, prefix);
	       octstr_destroy(xx);	       
	  }
	  
	  octstr_format_append(s, "</%sSenderIdentification>\n", prefix);
	  octstr_destroy(p);
	  octstr_destroy(q);
	  octstr_destroy(r);
     }

     p = octstr_create("");          
     if (mtag == MM7_TAG_SubmitReq ||
	 mtag == MM7_TAG_DeliverReq) { /* Multiple recipients,... */
	  output_rcpt("To", hdrs, p, prefix);	  
	  output_rcpt("Cc", hdrs, p, prefix);
	  output_rcpt("Bcc", hdrs, p, prefix);
	  
	  if (octstr_len(p) > 0)
	       octstr_format_append(s, "<%sRecipients>\n%S</%sRecipients>\n", prefix, p, prefix);     
     }  else if ((q = http_header_value(hdrs, octstr_imm("To"))) != NULL) {
	  append_address(p, q, prefix, 1);
	  octstr_format_append(s, "<%sRecipient>\n%S</%sRecipient>\n", prefix, p, prefix);
	  octstr_destroy(q);
     }
     octstr_destroy(p);     
     octstr_destroy(xmlns);

     /* cycle through rest of headers. */

     for (i = 0, n = gwlist_len(hdrs); i < n; i++) {
	  Octstr *h = NULL, *v  = NULL;
	  char *zz, *s1, *s2;
	  int tag;
	  int skip = 0;
	  
	  http_header_get(hdrs, i, &h, &v);
	  tag = mms_string_to_mm7tag(h);
	  zz = octstr_get_cstr(h);
	  
	  switch(tag) {
	  case MM7_TAG_MessageType:
	  case MM7_TAG_To:
	  case MM7_TAG_Cc:
	  case MM7_TAG_Bcc:	  
	  case MM7_TAG_Fault:	  
	  case MM7_TAG_faultstring:	  
	  case MM7_TAG_faultcode:	  
	  case MM7_TAG_VASID:	  	       
	  case MM7_TAG_VASPID:	  	       
	  case MM7_TAG_MM7Version:	  	       
	  case MM7_TAG_replyChargingSize:
	  case MM7_TAG_replyDeadline:
	  case MM7_TAG_TransactionID:
	  case MM7_TAG_StatusCode:
	  case MM7_TAG_StatusText:
	  case MM7_TAG_Details:
	  case MM7_TAG_SenderAddress:
	  case MM7_TAG_allowAdaptations:
	       skip = 1;
	       break;
	  case MM7_TAG_Sender:
	       p = octstr_create("");
	       append_address(p, v, prefix, ver_compare(ver,&min_ver) > 0);
	       
	       if (octstr_len(p) > 0)
		    octstr_format_append(s, "<%sSender>%S</%sSender>\n", prefix, p, prefix);
	       octstr_destroy(p);
	       skip = 1;
	       break;
	  case MM7_TAG_Content: 
	       if ((p = http_header_value(hdrs, octstr_imm("allowAdaptations"))) != NULL) 
		    p = octstr_format(" allowAdaptations=\"%S\"", p);
	       else 
		    p = octstr_imm("");
	       octstr_format_append(s, "<%sContent href=\"%S\"%S/>\n", prefix, v,p);
	       octstr_destroy(p);
	       skip = 1;
	       break;
	  case  MM7_TAG_ReplyCharging:
	       p =  http_header_value(hdrs, octstr_imm("replyChargingSize"));
	       q = http_header_value(hdrs, octstr_imm("replyDeadline"));
	       
	       octstr_format_append(s, "<%sReplyCharging", prefix);
	       if (p) {
		    octstr_format_append(s, " replyChargingSize=\"%S\"", p);
		    octstr_destroy(p);
	       }
	       if (q) {
		    octstr_format_append(s, " replyDeadline=\"%S\"", q);
		    octstr_destroy(q);
	       }
	       octstr_append_cstr(s, "/>\n");
	       skip = 1;
	       break;
	  case MM7_TAG_EarliestDeliveryTime:
	  case MM7_TAG_ExpiryDate:
	  case MM7_TAG_TimeStamp:
	  case MM7_TAG_Date:
	       t = date_parse_http(v);
	       octstr_destroy(v);
	       v = date_create_iso(t); /* format as ISO time... */
	       break;
	  case  MM7_TAG_Status:
	       p =  http_header_value(hdrs, octstr_imm("StatusCode"));
	       q = http_header_value(hdrs, octstr_imm("StatusText"));


	       octstr_format_append(s, "<%sStatus>\n", prefix);
	       if (p) {
		    octstr_format_append(s, "<%sStatusCode>%S</%sStatusCode>\n", prefix, p, prefix);
		    octstr_destroy(p);
	       }
	       if (q) {
		    octstr_format_append(s, "<%sStatusText>%S</%sStatusText>\n", prefix, q, prefix);
		    octstr_destroy(q);
	       }
	       q = http_header_value(hdrs, octstr_imm("Details"));
	       if (q) {
		    octstr_format_append(s, "<%sDetails>%S</%sDetails>\n", prefix, q, prefix);
		    octstr_destroy(q);
	       }

	       octstr_format_append(s, "</%sStatus>\n", prefix);
	       skip = 1;
	       break;
	  case MM7_TAG_UACapabilities:
	       s1 = octstr_get_cstr(v);
	       if ((s2 = strrchr(s1, ',')) != NULL) {
		    t = strtoul(s2 + 1, NULL, 10);
		    octstr_delete(v, s2-s1, octstr_len(v));
	       } else 
		    t = time(NULL);
	       p = date_create_iso(t);
	       octstr_format_append(s, "<%s%S TimeStamp=\"%S\" UAProf=\"%S\"/>\n",
				    prefix, h, p, v);
	       octstr_destroy(p);
	       skip = 1;
	       break;
	  default:	       
	       break;
	  }
	  
	  if (!skip && h && v)
	       octstr_format_append(s, "<%s%S>%S</%s%S>\n", prefix, h, v, prefix, h);
	  
	  octstr_destroy(h);
	  octstr_destroy(v);
     }
     octstr_format_append(s, "</%s%S>\n", prefix, mtype);
     octstr_destroy(mtype);     
     if (fault) {	  
	  octstr_append_cstr(s, "</detail>\n");
	  octstr_append_cstr(s, "</" SOAP_ENV ":Fault>\n");
	  octstr_destroy(fault);
     }
     
     octstr_append_cstr(s, "</" SOAP_ENV ":Body>\n");
     octstr_append_cstr(s, "</" SOAP_ENV ":Envelope>\n");
     
     return s;
}

int mm7_soapmsg_to_httpmsg(MSoapMsg_t *m, MM7Version_t *ver, List **hdrs, Octstr **body)
{
     MIMEEntity *mime;
     Octstr *ctype = NULL, *s; 
     List *headers;
     
     mime = mime_entity_create();
    
     if (m->msg) {
	  MIMEEntity *c = mime_entity_duplicate(m->msg);
	  Octstr *cloc = octstr_format("cid:c%ld.%d.%c%c.msg", 
				       time(NULL), random(), 
				       'A' + random() % 26,
				       'a' + random() % 26);
	  Octstr  *cloc_str = octstr_format("<%s>", octstr_get_cstr(cloc) + 4);
	  Octstr *envloc  = octstr_format("<s%ld.%d.%c%c.msg>", 
				       time(NULL), random(), 
				       'A' + random() % 26,
				       'a' + random() % 26);
	  MIMEEntity *xml = mime_entity_create();
	  List *hh = http_header_duplicate(m->envelope);
	  List *hx;
	  
	  /* Replace in envelope. */
	  http_header_remove_all(hh, "Content");
	  http_header_add(hh, "Content", octstr_get_cstr(cloc));
	  
	  /* Replace content location in msg part. */
	  hx = mime_entity_headers(c);
	  http_header_remove_all(hx, "Content-ID");
	  http_header_add(hx, "Content-ID", octstr_get_cstr(cloc_str));
	  mime_replace_headers(c, hx);
	  http_destroy_headers(hx);

	  hx = http_create_empty_headers();
	  http_header_add(hx, "Content-Type", "text/xml");
	  http_header_add(hx, "Content-ID",  octstr_get_cstr(envloc));
	  mime_replace_headers(xml, hx);
	  http_destroy_headers(hx);

	  s =  headers_to_soapxml(hh,ver);	  
	  mime_entity_set_body(xml, s);
	  octstr_destroy(s);

	  mime_entity_add_part(mime, xml);
	  mime_entity_add_part(mime, c);

	  http_destroy_headers(hh);

	  ctype = octstr_format("multipart/related; type=\"text/xml\"; start=\"%S\"",
				envloc);
	  
	  octstr_destroy(envloc);
	  octstr_destroy(cloc);				
	  octstr_destroy(cloc_str); 

	  mime_entity_destroy(xml);
	  mime_entity_destroy(c); 
     } else {
	  ctype = octstr_imm("text/xml");
	  s = headers_to_soapxml(m->envelope, ver);
	  mime_entity_set_body(mime,s);
	  octstr_destroy(s);
     }
     
     headers = http_create_empty_headers();
     http_header_add(headers, "Content-Type", octstr_get_cstr(ctype));
     http_header_add(headers, "SOAPAction", "\"\"");
     mime_replace_headers(mime, headers);
     http_destroy_headers(headers);

     *body = mime_entity_body(mime);
     *hdrs = mime_entity_headers(mime);
#if 0
     debug("mms2soap", 0, "SOAP headers dump follows: ");
     http_header_dump(*hdrs);
     debug("mms2soap", 0, "SOAP MSG is: %s", octstr_get_cstr(*body));
#endif
     mime_entity_destroy(mime);
     octstr_destroy(ctype);

     return 0;
}

int mm7_msgtype(MSoapMsg_t *m)
{
     Octstr *typ = http_header_value(m->envelope, octstr_imm("MessageType"));
     int ret;
     
     if (!typ) 
	  return -1;
     
     ret = mms_string_to_mm7tag(typ);
     octstr_destroy(typ);

     return ret;
}

static int get_rcptvalues(List *to, List *headers, char *hname)
{
     List *l;
     int i, n;
     
     l = http_header_find_all(headers, hname);
     
     for (i = 0, n = (l) ? gwlist_len(l) : 0; i < n; i++) {
	  Octstr *h, *v;
	  int ch;
	  http_header_get(l, i, &h, &v);
	  
	  ch = octstr_get_char(v, 0);
	  if (ch == '+')
	       gwlist_append(to, octstr_copy(v, 2, octstr_len(v)));
	  octstr_destroy(h);
	  octstr_destroy(v);
     }
     http_destroy_headers(l);
     return 0;
}

int mm7_get_envelope(MSoapMsg_t *m, 
		     Octstr **sender,
		     List **to, Octstr **subject, 
		     Octstr **vasid, 	       
		     time_t *expiry_t, 
		     time_t *delivery_t,
		     Octstr **uaprof,
		     time_t *uaprof_tstamp)
{
     Octstr *s;
     
     if (*to == NULL)
	  *to = gwlist_create();
     
     get_rcptvalues(*to, m->envelope, "To");
     get_rcptvalues(*to, m->envelope, "Cc");
     get_rcptvalues(*to, m->envelope, "Bcc");

     
     *subject = http_header_value(m->envelope, octstr_imm("Subject"));
     *vasid = http_header_value(m->envelope, octstr_imm("VASID"));

     if ((s = http_header_value(m->envelope, octstr_imm("SenderAddress"))) == NULL)
	  s = http_header_value(m->envelope, octstr_imm("Sender"));

     if (s && octstr_get_char(s, 0) == '+')
	  octstr_delete(s, 0, 2);
     else if (s) {
	  octstr_destroy(s);
	  s = NULL;
     }     
     *sender = s;
     
     if (expiry_t) {
	  Octstr *s = http_header_value(m->envelope, octstr_imm("ExpiryDate"));
	  
	  *expiry_t = -1; 
	  if (s) {
	       *expiry_t = date_parse_http(s);
	       octstr_destroy(s);
	  }
     }

     if (delivery_t) {
	  Octstr *s = http_header_value(m->envelope, octstr_imm("EarliestDeliveryTime"));	  
	  *delivery_t = -1; 
	  if (s) {
	       *delivery_t = date_parse_http(s);
	       octstr_destroy(s);
	  }
     }     
     if (uaprof) {
	  Octstr *s = http_header_value(m->envelope, octstr_imm("UACapabilities"));	  
	  if (uaprof_tstamp) *uaprof_tstamp = -1;
	  if (s) {
	       char *s1 = octstr_get_cstr(s);
	       char *s2 = strrchr(s1, ',');

	       if (s2) {
		    *uaprof_tstamp = strtoul(s2+1, NULL, 10);
		    octstr_delete(s, s2-s1, octstr_len(s));
	       }
	       *uaprof = s;
	  }
     }
     return 0;
}


MmsMsg *mm7_soap_to_mmsmsg(MSoapMsg_t *m, Octstr *from)
{

     int msgtype = mm7_msgtype(m);
     MmsMsg *msg = NULL;
     Octstr *s, *f, *p, *q, *r;
     List *l;
     time_t t;
     
     switch(msgtype) {
     case MM7_TAG_DeliverReq:
     case MM7_TAG_SubmitReq:
     case MM7_TAG_ReplaceReq:
	  msg = mms_frommime(m->msg);
	  
	  if (!msg)
	       break;
	  mms_remove_headers(msg, "Content-ID"); /* not necessary, but... */
	  /* Put in some headers... */
	  if (from)
	       mms_replace_header_value(msg, "From", octstr_get_cstr(from));
	  
	  if ((s = http_header_value(m->envelope, octstr_imm("MessageClass"))) != NULL) {
	       if (mms_string_to_message_class(s) >= 0)
		    mms_replace_header_value(msg, "X-Mms-Message-Class", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }

	  if ((s = http_header_value(m->envelope, octstr_imm("Priority"))) != NULL) {
	       if (mms_string_to_priority(s) >= 0)
		    mms_replace_header_value(msg, "X-Mms-Priority", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }
	  
	  if ((s = http_header_value(m->envelope, octstr_imm("DeliveryReport"))) != NULL) {
	       long x = mms_string_to_boolean_flag(s);
	       if (x >= 0)
		    mms_replace_header_value(msg, "X-Mms-Delivery-Report", 
					  (char *)mms_reports_to_cstr(x));
	       octstr_destroy(s);
	  }

	  if ((s = http_header_value(m->envelope, octstr_imm("ReadReply"))) != NULL) {
	       long x = mms_string_to_boolean_flag(s);
	       if (x >= 0)
		    mms_replace_header_value(msg, "X-Mms-Read-Report", 
					     (char *)mms_reports_to_cstr(x));
	       octstr_destroy(s);
	  }
	  
	  if ((s = http_header_value(m->envelope, octstr_imm("TimeStamp"))) != NULL) {
	       mms_replace_header_value(msg, "Date", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }
	  
	  if ((s = http_header_value(m->envelope, octstr_imm("ExpiryDate"))) != NULL) {
	       mms_replace_header_value(msg, "X-Mms-Expiry", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }
	  
	  if ((s = http_header_value(m->envelope, octstr_imm("Subject"))) != NULL) {
	       mms_replace_header_value(msg, "Subject", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }

	  if ((s = http_header_value(m->envelope, octstr_imm("MessageID"))) != NULL) {
	       mms_replace_header_value(msg, "Message-ID", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }
	  
	  /* Put in recipient list. XXX - really?? */
	  l = gwlist_create();
	  get_rcptvalues(l, m->envelope, "To");
	  mms_replace_header_values(msg, "To", l);
	  gwlist_destroy(l,  (gwlist_item_destructor_t *)octstr_destroy);

	  l = gwlist_create();
	  get_rcptvalues(l, m->envelope, "Cc");
	  mms_replace_header_values(msg, "Cc", l);
	  gwlist_destroy(l,  (gwlist_item_destructor_t *)octstr_destroy);
	  
	  /* XXX - we ignore reply charging, etc. */
	  break;

     case MM7_TAG_DeliveryReportReq: /* should we bother?? Can these ever be handled here?? */
     case MM7_TAG_ReadReplyReq:
	  s = http_header_value(m->envelope, octstr_imm("MessageID"));
	  if ((p = http_header_value(m->envelope, octstr_imm("To"))) != NULL && 
	      octstr_get_char(p, 0) == '+')
	       octstr_delete(p, 0, 2);
	  else if (p) {
	       octstr_destroy(p);
	       p = NULL;
	  }

	  if ((f = http_header_value(m->envelope, octstr_imm("From"))) != NULL && 
	      octstr_get_char(f, 0) == '+')
	       octstr_delete(f, 0, 2);
	  else if (f) {
	       octstr_destroy(f);
	       f = NULL;
	  }
	  
	  if ((q = http_header_value(m->envelope, octstr_imm("Date"))) != NULL)
	       t = date_parse_http(q);
	  else 
	       t = time(NULL);

	  r = http_header_value(m->envelope, octstr_imm("MMStatus"));
	  if (msgtype == MM7_TAG_DeliveryReportReq && p) { 
	       Octstr *status = r && (mms_string_to_status(r) >= 0) ? r : octstr_imm("Indeterminate");
	       msg = mms_deliveryreport(s ? s : octstr_imm("0000"), from, p, t, status);
	  } else if (msgtype ==  MM7_TAG_ReadReplyReq && 
		   f && p && r && mms_string_to_read_status(r) >= 0)
	       msg = mms_readreport(s ? s : octstr_imm("0000"),
				    f, p, t, r);    

	  octstr_destroy(s);
	  octstr_destroy(p);
	  octstr_destroy(r);
	  octstr_destroy(f);
	  octstr_destroy(q);
	  break;
     default:
	  break;
     }
     return msg;
}

void mm7_soap_destroy(MSoapMsg_t *m)
{
     if (m == NULL) return;
     
     http_destroy_headers(m->envelope);
     if (m->msg)
	  mime_entity_destroy(m->msg);
     gw_free(m);     
}

static MSoapMsg_t *mm7_soap_create(int msgtype, Octstr *otransid)
{
     MSoapMsg_t *m = gw_malloc(sizeof *m);
     
     m->envelope = http_create_empty_headers();
     m->msg = NULL;

     http_header_add(m->envelope, "MessageType", (char *)mms_mm7tag_to_cstr(msgtype));
     /* http_header_add(m->envelope, "MM7Version", MM7_DEFAULT_VERSION); */
     http_header_add(m->envelope, "TransactionID", 
		     otransid ? octstr_get_cstr(otransid) : "0000");     
     return m;
}

MSoapMsg_t *mm7_mmsmsg_to_soap(MmsMsg *msg, Octstr *from, List *xto, 
			       Octstr *transid, Octstr *srvcode,
			       Octstr *linkedid, int isclientside, 
			       char *vaspid, char *vasid, 
			       Octstr *uaprof,
			       time_t uaprof_tstamp,
			       List *hdrs)
{
     int i, n, mtype = mms_messagetype(msg);
     MSoapMsg_t *m = NULL;
     Octstr *xfrom = (from != NULL) ?  octstr_format("+ %S", from) : NULL, *s;
     Octstr *xuaprof_val = (uaprof) ? octstr_format("%S,%ld", uaprof, uaprof_tstamp) : NULL;

     switch(mtype) {
     case MMS_MSGTYPE_SEND_REQ:
     case MMS_MSGTYPE_RETRIEVE_CONF:
	  m = mm7_soap_create(isclientside ? MM7_TAG_SubmitReq : MM7_TAG_DeliverReq, 
			      transid);	  
	  m->msg = mms_tomime(msg,1);
	  
	  strip_non_essential_headers(m->msg);

	  for (i = 0, n = xto ? gwlist_len(xto) : 0; i < n; i++) { /* Add recipients. */
	       Octstr *xx = octstr_format("+ %S", gwlist_get(xto, i));
	       http_header_add(m->envelope, "To",
			       octstr_get_cstr(xx));
	       octstr_destroy(xx);
	  }

	  if (srvcode)
	       http_header_add(m->envelope, 
			       isclientside ? "ServiceCode" : "MMSRelayServerID", 
			       octstr_get_cstr(srvcode));

	  if (linkedid)
	       http_header_add(m->envelope, "LinkedID", octstr_get_cstr(linkedid));
	  if (xfrom)
	       http_header_add(m->envelope, 
			       isclientside ? "SenderAddress" : "Sender", 
			       octstr_get_cstr(xfrom));	  


	  if (isclientside) {
	       Octstr *distrib_indicator = hdrs ?  http_header_value(hdrs, octstr_imm("X-Mbuni-DistributionIndicator")) : NULL;
	       Octstr *cparty = hdrs ?  http_header_value(hdrs, octstr_imm("X-Mbuni-Charged-Party")) : NULL;
	       if (vaspid)
		    http_header_add(m->envelope, "VASPID", vaspid);
	       if (vasid)
		    http_header_add(m->envelope, "VASID", vasid);

	       if ((s = mms_get_header_value(msg, octstr_imm("X-Mms-Message-Class"))) != NULL) {
		    http_header_add(m->envelope, "MessageClass", octstr_get_cstr(s));	       
		    octstr_destroy(s);
	       }

	       /* Track adaptations stuff -- Vincent */
               if ((s = mms_get_header_value(msg, octstr_imm("X-Mms-Allow-Adaptations"))) != NULL) {
                    char *val = (octstr_case_compare(s, octstr_imm("true")) == 0) ?
                         "true" : "false";
                    http_header_add(m->envelope, "allowAdaptations", val);
                    octstr_destroy(s);
               }
	       
	       if (distrib_indicator)
		    http_header_add(m->envelope, "DistributionIndicator",
				    octstr_str_case_compare(distrib_indicator, "true") == 0 ? "true" : "false");
	       if (cparty)
		    http_header_add(m->envelope, "ChargedParty",
				    octstr_get_cstr(cparty));

	       octstr_destroy(distrib_indicator);
	       octstr_destroy(cparty);
	  } else { /* not clientside. */	       
	       if (xuaprof_val) /* only on DeliverReq. */
		    http_header_add(m->envelope, "UACapabilities", octstr_get_cstr(xuaprof_val));	       
	  }

	  if ((s = mms_get_header_value(msg, octstr_imm("Date"))) != NULL) {
	       http_header_add(m->envelope, "TimeStamp", octstr_get_cstr(s));	       
	       octstr_destroy(s);
	  }

	  if (isclientside) { /* We need this twice because of stupid parsers out there that
			       * care about the order of XML fields. 
			       */
	       if ((s = mms_get_header_value(msg, octstr_imm("X-Mms-Expiry"))) != NULL) {
		    http_header_add(m->envelope, "ExpiryDate", octstr_get_cstr(s));	       
		    octstr_destroy(s);
	       }
	       
	       if ((s = mms_get_header_value(msg, octstr_imm("X-Mms-Delivery-Report"))) != NULL) {
		    char *val = (octstr_case_compare(s, octstr_imm("Yes")) == 0) ? 
			 "true" : "false";
		    http_header_add(m->envelope, "DeliveryReport", val);	       
		    octstr_destroy(s);
	       }	       

	       if ((s = mms_get_header_value(msg, octstr_imm("X-Mms-Read-Report"))) != NULL) {
		    char *val = (octstr_case_compare(s, octstr_imm("Yes")) == 0) ? 
			 "true" : "false";
		    http_header_add(m->envelope, "ReadReply", val);	       
		    octstr_destroy(s);
	       }	       

	  }

	  if ((s = mms_get_header_value(msg, octstr_imm("X-Mms-Priority"))) != NULL) {
	       http_header_add(m->envelope, "Priority", octstr_get_cstr(s));	       
	       octstr_destroy(s);
	  }

	  if ((s = mms_get_header_value(msg, octstr_imm("Subject"))) != NULL) {
	       http_header_add(m->envelope, "Subject", octstr_get_cstr(s));	       
	       octstr_destroy(s);
	  }

#if 0 /* handled above. */
	  /* Should we bother to strip message part of headers??? */
	  headers = mime_entity_headers(m->msg);
	  http_header_remove_all(headers, "Subject");
	  http_header_remove_all(headers, "X-Mms-Message-Type");
	  http_header_remove_all(headers, "X-Mms-Message-Version");
          http_header_remove_all(headers, "X-Mms-Allow-Adaptations");
	  http_header_remove_all(headers, "From");
	  http_header_remove_all(headers, "To");
	  http_header_remove_all(headers, "Cc");
	  http_header_remove_all(headers, "Bcc");
	  mime_replace_headers(m->msg, headers);
	  http_destroy_headers(headers);
#endif
	  break;
     case MMS_MSGTYPE_READ_ORIG_IND:
     case MMS_MSGTYPE_DELIVERY_IND:
	  m = mm7_soap_create((mtype == MMS_MSGTYPE_READ_ORIG_IND) ? 
			      MM7_TAG_ReadReplyReq : MM7_TAG_DeliveryReportReq, 
			      transid);	  

	  if (xfrom)
	       http_header_add(m->envelope, "Sender", octstr_get_cstr(xfrom));	  

	  if (xto && gwlist_len(xto) > 0) {
	       Octstr *xx = octstr_format("+ %S", gwlist_get(xto, 0));
	       http_header_add(m->envelope, "To",
			       octstr_get_cstr(xx));
	       octstr_destroy(xx);
	  }

	  s = mms_get_header_value(msg, octstr_imm("Message-ID"));
	  if (!s)
	       s = octstr_duplicate(transid);
	  http_header_add(m->envelope, "MessageID", octstr_get_cstr(s));
	  octstr_destroy(s);

	  if ((s = mms_get_header_value(msg, octstr_imm("Date"))) != NULL) {
	       http_header_add(m->envelope, 
			       (mtype == MMS_MSGTYPE_READ_ORIG_IND) ? "TimeStamp" : "Date", 
			       octstr_get_cstr(s));	       
	       octstr_destroy(s);
	  }
	  
	  if ((s = mms_get_header_value(msg, 
					(mtype == MMS_MSGTYPE_READ_ORIG_IND) ? 	
					octstr_imm("X-Mms-Read-Status") : 
					octstr_imm("X-Mms-Status") )) != NULL) {
	       http_header_add(m->envelope, "MMStatus", octstr_get_cstr(s));	       
	       octstr_destroy(s);
	  }
	  
	  if (mtype == MMS_MSGTYPE_DELIVERY_IND && 
	      xuaprof_val) /* and for DeliveryReport. */
	       http_header_add(m->envelope, "UACapabilities", octstr_get_cstr(xuaprof_val));	       

	  break;
     default:
	  break;
     }

     octstr_destroy(xfrom);
     octstr_destroy(xuaprof_val);


     return m;
}

MSoapMsg_t *mm7_make_resp(MSoapMsg_t *mreq, int status, Octstr *msgid, int isclientside)
{
     int mtype = mreq ? mm7_msgtype(mreq) : -1;
     Octstr *transid;
     MSoapMsg_t *mresp;
     char scode[64];
     unsigned char *statustxt = mms_soap_status_to_cstr(status);

     transid =  mreq ?   http_header_value(mreq->envelope, octstr_imm("TransactionID")) : NULL;
     
     if (!MM7_SOAP_STATUS_OK(status) || mtype < 0)
	  mresp = mm7_soap_create(isclientside ? MM7_TAG_VASPErrorRsp : MM7_TAG_RSErrorRsp, transid);
     else 
	  mresp = mm7_soap_create(mtype + 1, transid);

     sprintf(scode, "%d", status);
     http_header_add(mresp->envelope, "Status", ""); /* So that we get output. */
     http_header_add(mresp->envelope, "StatusCode", scode);
     http_header_add(mresp->envelope, "StatusText", statustxt ? (char *)statustxt : "Error");	  
     
     if (msgid && mtype == MM7_TAG_SubmitReq) /* only allow MessageID in submitRsp */
	  http_header_add(mresp->envelope, "MessageID", octstr_get_cstr(msgid));	  
     
     if (!MM7_SOAP_STATUS_OK(status)) {
	  http_header_add(mresp->envelope, "Fault", "");	 /* do we really need these? XXX */ 	  
	  http_header_add(mresp->envelope, "faultcode", "" SOAP_ENV ":Server");	  	  
	  http_header_add(mresp->envelope, "faultstring", "Server error");	  	  
     }

     octstr_destroy(transid);
     return mresp;
}

Octstr *mm7_soap_header_value(MSoapMsg_t *m, Octstr *header)
{
     return http_header_value(m->envelope, header);
}

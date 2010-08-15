/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * User-Agent profiles handling, content adaptation.
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#include <ctype.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "mms_uaprof.h"
#include "mms_util.h"

struct MmsUaProfile {
     List *versions;
     unsigned long maxmsgsize;
     struct {
	  long x, y;
     } maxres;
     struct {
	  unsigned char all;
	  unsigned char presentation;
	  List *content, *_hash; /* List of accepted content formats (+ hash keys for fast look-up). */
	  
	  List *charset, *_chash; /* List of accepted charsets. */
	  List *lang;    /* List of accepted languages. */
	  List *enc;     /* List of accepted encodings. */
     } ccppaccept;
};

static Dict *profile_dict; /* Of MmsUaProfile *. */
static Octstr *profile_dir;  /* Directory for storing data. */

/* Hash function -- case insensitive. */
static unsigned long hash_key(Octstr *s)
{
     unsigned long h = 0;
     int i, n;
     char *x;

     if (!s) return 0;
     for (x = octstr_get_cstr(s), i = 0, n = octstr_len(s); i<n; i++)
	  h  += (unsigned long)tolower(x[i]);
     return h;
}


static void destroy_uaprof(MmsUaProfile *prof)
{
     if (prof->versions)
	  gwlist_destroy(prof->versions, 
		       (gwlist_item_destructor_t *)octstr_destroy);
     
     if (prof->ccppaccept.content) {
	  gwlist_destroy(prof->ccppaccept.content, (gwlist_item_destructor_t *)octstr_destroy);
	  gwlist_destroy(prof->ccppaccept._hash, NULL);
     }
     
     if (prof->ccppaccept.charset) {
	  gwlist_destroy(prof->ccppaccept.charset, (gwlist_item_destructor_t *)octstr_destroy);
	  gwlist_destroy(prof->ccppaccept._chash, NULL);
     } if (prof->ccppaccept.lang)
	  gwlist_destroy(prof->ccppaccept.lang, (gwlist_item_destructor_t *)octstr_destroy);
     if (prof->ccppaccept.enc)
	  gwlist_destroy(prof->ccppaccept.enc, (gwlist_item_destructor_t *)octstr_destroy);

     gw_free(prof);
}

static void dump_profile(MmsUaProfile *prof, Octstr *name)
{
     int i;
     Octstr *s;
     
     debug("mms.uaprof", 0, "Dumping profile for %s", octstr_get_cstr(name));
     
     debug("mms.uaprof", 0, "MaxMsgSize: %ld", prof->maxmsgsize);
     debug("mms.uaprof", 0, "MaxRes: %ldx%ld", prof->maxres.x,prof->maxres.y);
     

     s = octstr_create("");
     if (prof->ccppaccept.content)
	  for (i=0; i<gwlist_len(prof->ccppaccept.content); i++)
	       octstr_format_append(s, "%S, ", gwlist_get(prof->ccppaccept.content,i));
     debug("mms.uaprof", 0, "Accept content: %s", octstr_get_cstr(s));     
     octstr_destroy(s);


     s = octstr_create("");
     if (prof->ccppaccept.enc)
	  for (i=0; i<gwlist_len(prof->ccppaccept.enc); i++)
	       octstr_format_append(s, "%S, ", gwlist_get(prof->ccppaccept.enc,i));
     debug("mms.uaprof", 0, "Accept encodings: %s", octstr_get_cstr(s));     
     octstr_destroy(s);

     s = octstr_create("");
     if (prof->ccppaccept.lang)
	  for (i=0; i<gwlist_len(prof->ccppaccept.lang); i++)
	       octstr_format_append(s, "%S, ", gwlist_get(prof->ccppaccept.lang,i));
     debug("mms.uaprof", 0, "Accept language: %s", octstr_get_cstr(s));     
     octstr_destroy(s);

     s = octstr_create("");
     if (prof->ccppaccept.charset)
	  for (i=0; i<gwlist_len(prof->ccppaccept.charset); i++)
	       octstr_format_append(s, "%S, ", gwlist_get(prof->ccppaccept.charset,i));
     debug("mms.uaprof", 0, "Accept charset: %s", octstr_get_cstr(s));     
     octstr_destroy(s);

     s = octstr_create("");
     if (prof->versions)
	  for (i=0; i<gwlist_len(prof->versions); i++)
	       octstr_format_append(s, "%S, ", gwlist_get(prof->versions,i));
     debug("mms.uaprof", 0, "Mms Version: %s", octstr_get_cstr(s));     
     octstr_destroy(s);
     
}

/* Helper function: find a node. Uses breadth first search */
static xmlNodePtr find_node(xmlNodePtr start, char *name, char *id, int level, int maxlevel)
{
     xmlNodePtr node, x, list;

     
     if (level >= maxlevel) return NULL;
     
     /* First search at top level. */
     for (list=start; list; list=list->next)
	  if (list->type == XML_COMMENT_NODE)
	       continue;
	  else if (xmlStrcasecmp(list->name, (const xmlChar *)name) == 0) {
	       if (!id)
		    return list;
	       else {
		    unsigned char *s;
		    if ((s= xmlGetProp(list,(unsigned char *)"ID")) != NULL && 
			xmlStrcasecmp(s,(unsigned char *)id) == 0) {
			 xmlFree(s);
			 return list;
		    }
		    if (s) xmlFree(s);
	       }
	  }
     /* Then recurse...*/
     for (list = start; list; list=list->next)     
	  for (node = list->xmlChildrenNode; node; node = node->next)
	       if (xmlStrcasecmp(node->name, (const xmlChar *)name) == 0) {
		    if (!id)
			 return node;
		    else {
			 unsigned char *s;
			 if ((s = xmlGetProp(node,(unsigned char *)"ID")) != NULL && 
			     xmlStrcasecmp(s,(unsigned char *)id) == 0) {
			      xmlFree(s);
			      return node; 
			 }
			 if (s) xmlFree(s);
		    }
	       } else if (node->type != XML_COMMENT_NODE && 
			  (x = find_node(node, name,id, level+1,maxlevel)) != NULL) 
		    return x;     
     return NULL;
}

MmsUaProfile *mms_make_ua_profile(List *req_headers)
{
     MmsUaProfile *prof = NULL;
     Octstr *s, *ua;
     List *l;
     int i, n;
     static int uacounter;
     
     /* Check cache first, if not, then construct. */
     if ((ua = http_header_value(req_headers, octstr_imm("User-Agent"))) == NULL)
	  ua = octstr_format("dummy-ua-%d", uacounter++);
     
     if ((prof = dict_get(profile_dict, ua)) != NULL) 
	  goto done;
          
     prof = gw_malloc(sizeof *prof);
     memset(prof, 0, sizeof *prof);
     
     /* Put in some defaults. then read then check accepts. */
     prof->maxres.x = 640;
     prof->maxres.y = 480;
     prof->maxmsgsize = 100*1024;
     prof->versions = gwlist_create();


     gwlist_append(prof->versions, octstr_imm("1.0")); /* Assume 1.0 for now. */
     
     /* Get accepted charsets. */
     s = http_header_value(req_headers, octstr_imm("Accept-Charset"));
     
     if (s && (l = http_header_split_value(s)) != NULL) {
	  prof->ccppaccept.charset = l;
	  prof->ccppaccept._chash = gwlist_create();
	  for (i = 0, n = gwlist_len(l); i<n; i++)
	       gwlist_append(prof->ccppaccept._chash, (void *)hash_key(gwlist_get(l, i)));	  
     }
     if (s) octstr_destroy(s);


     /* Get accepted encodings. */
     s = http_header_value(req_headers, octstr_imm("Accept-Encoding"));
     
     if (s && (l = http_header_split_value(s)) != NULL)
	  prof->ccppaccept.enc = l;     

     if (s) octstr_destroy(s);



     /* Get accepted language. */
     s = http_header_value(req_headers, octstr_imm("Accept-Language"));   
     if (s && (l = http_header_split_value(s)) != NULL)
	  prof->ccppaccept.lang = l;     

     if (s) octstr_destroy(s);
     
     s = http_header_value(req_headers, octstr_imm("Accept"));   
     if (s && (l = http_header_split_value(s)) != NULL) {
	  prof->ccppaccept.content = l;
	  prof->ccppaccept._hash = gwlist_create();
	  
	  for (i = 0, n = l ? gwlist_len(l) : 0; i<n; i++) {
	       Octstr *x = gwlist_get(l, i);
	       if (octstr_str_compare(x, "*/*") == 0)
		    prof->ccppaccept.all = 1;
	       else if (octstr_case_compare(x, octstr_imm(PRES_TYPE)) == 0)
		    prof->ccppaccept.presentation = 1;
	       
	       gwlist_append(prof->ccppaccept._hash, (void *)hash_key(x));
	  }
     }
     
     if (s) octstr_destroy(s);
 
     /* Put it in with the UA string as the key. */
     if (dict_put_once(profile_dict, ua, prof) != 1)
	  mms_warning(0, "mms_uaprof", NULL, "Duplicate cache entry(%s)?\n", 
		  octstr_get_cstr(ua));
     
     /* Done. Dump it while debugging. */
 done:

#if 1
     dump_profile(prof, ua ? ua : octstr_imm("<from http headers>"));
#endif
     
     if (ua)
	  octstr_destroy(ua);
     return prof;     
}

static MmsUaProfile *parse_uaprofile(Octstr *xml)
{
     char *s = octstr_get_cstr(xml);
     xmlDocPtr doc = xmlParseMemory(s, octstr_len(xml));
     xmlNodePtr node, xnode;
     MmsUaProfile *prof = NULL;
     
     if (!doc || !doc->xmlChildrenNode) 
	  goto done;

     node = find_node(doc->xmlChildrenNode, "Description", "MmsCharacteristics",0,3);
     
     prof = gw_malloc(sizeof *prof);
     memset(prof, 0, sizeof *prof);
     
     /* Put in some defaults. then read the file. */
     prof->versions = NULL;
     prof->maxres.x = 640;
     prof->maxres.y = 480;
     prof->maxmsgsize = 100*1024;
     prof->versions = NULL;


     if (!node) 
	  goto done;

     for (xnode = node->xmlChildrenNode; xnode; xnode = xnode->next) {
	  xmlNodePtr child = xnode->xmlChildrenNode, lnode, rdfnode;
	  const unsigned char *xname = xnode->name;
	  unsigned char *childtext = xmlNodeListGetString(doc, child, 1);
	  List *l;
	  
	  /* If there is a Bag, get the list. */
	  if ((rdfnode = find_node(xnode->xmlChildrenNode, "Bag", NULL,0,1)) != NULL) {
	       l = gwlist_create();	       
	       for (lnode = rdfnode->xmlChildrenNode; lnode; lnode = lnode->next)
		    if (xmlStrcasecmp(lnode->name, (const xmlChar *)"li") == 0) {
			 unsigned char *t = xmlNodeListGetString(doc, lnode->xmlChildrenNode,1);
			 if (t) {
			   gwlist_append(l, octstr_create((char *)t));
			   xmlFree(t);
			 }
		    }
	  } else
	       l = NULL;
	  
	  if (xmlStrcasecmp(xname, (const xmlChar *)"MmsMaxMessageSize") == 0) 
	    sscanf((char *)childtext, "%ld", &prof->maxmsgsize);
	  else if (xmlStrcasecmp(xname, (const xmlChar *)"MmsMaxImageResolution") == 0) 
	    sscanf((char *)childtext, "%ldx%ld", &prof->maxres.x, &prof->maxres.y);
	  else if (xmlStrcasecmp(xname, (const xmlChar *)"MmsCcppAcceptCharSet") == 0 ||
		   xmlStrcasecmp(xname, (const xmlChar *)"MmsCcppAccept-CharSet") == 0) {/* Cranky old ones! */
	       int i, n;
	       prof->ccppaccept.charset = l;
	       prof->ccppaccept._chash = gwlist_create();
	       for (i = 0, n = gwlist_len(l); i<n; i++)
		    gwlist_append(prof->ccppaccept._chash, (void *)hash_key(gwlist_get(l, i)));
	  } else if (xmlStrcasecmp(xname, (const xmlChar *)"MmsCcppAcceptLanguage") == 0) 
	       prof->ccppaccept.lang = l;
	  else if (xmlStrcasecmp(xname, (const xmlChar *)"MmsCcppAcceptEncoding") == 0) 
	       prof->ccppaccept.enc = l;
	  else if (xmlStrcasecmp(xname, (const xmlChar *)"MmsVersion") == 0) {
	       if (!l && childtext) { /* SonyEriccson uses old format! */
		    l = gwlist_create();
		    gwlist_append(l, octstr_create((char *)childtext));
	       }
	       prof->versions = l;
	  } else if (xmlStrcasecmp(xname, (const xmlChar *)"MmsCcppAccept") == 0) {
	       int i, n;
	       prof->ccppaccept.content = l;
	       prof->ccppaccept._hash = gwlist_create();
	       
	       for (i = 0, n = l ? gwlist_len(l) : 0; i<n; i++) {
		    Octstr *x = gwlist_get(l, i);
		    if (octstr_str_compare(x, "*/*") == 0)
			 prof->ccppaccept.all = 1;
		    else if (octstr_case_compare(x, octstr_imm(PRES_TYPE)) == 0)
			 prof->ccppaccept.presentation = 1;

		    gwlist_append(prof->ccppaccept._hash, (void *)hash_key(x));
	       }
	  }	  
	  if (childtext) xmlFree(childtext);
     }

 done:
     if (doc) xmlFreeDoc(doc);
     return prof;
}


static int replace_slash(int ch)
{
     return (ch == '/') ? '$' : ch;
}

static int unreplace_slash(int ch)
{
     return (ch == '$') ? '/' : ch;
}

static int mms_load_ua_profile_cache(char *dir)
{

     DIR *dirp;
     struct dirent *dp;
     dirp = opendir(dir);
     
     if (!dirp) {
	  mms_error(0, "mms_uaprof", NULL, "Failed to open UA prof cache directory %s", 
		dir);
	  return -1;
     }

     while ((dp = readdir(dirp)) != NULL) {
	  Octstr *fname;
	  Octstr *xml = NULL;
	  MmsUaProfile *prof = NULL;
	  Octstr *key = NULL;
	  
	  
	  if (strcmp(dp->d_name, ".") == 0 || 
	      strcmp(dp->d_name, "..") == 0) /* A directory, skip. */
	       continue;
	  
	  fname = octstr_format("%.255s/%.254s", dir, dp->d_name);
	  
	  xml = octstr_read_file(octstr_get_cstr(fname));
	  octstr_destroy(fname);
	  if (!xml) {
	       mms_error(0, "mms_uaprof", NULL, "Failed to read UA prof doc %s in %s (%s)\n", 
		     dp->d_name, dir, strerror(errno));
	       continue;
	  }
	  
	  prof = parse_uaprofile(xml);
	  if (!prof) {
	       mms_error(0, "mms_uaprof", NULL, "Failed to parse UA prof doc %s in %s\n", dp->d_name, dir);
	       goto loop;
	  }

	  key = octstr_create(dp->d_name);
	  octstr_convert_range(key, 0, octstr_len(key), unreplace_slash);

	  if (dict_put_once(profile_dict, key, prof) != 1)
	       mms_warning(0, "mms_uaprof", NULL,  "Duplicate cache entry(%s)?\n", 
		       octstr_get_cstr(key));
#if 1
	  dump_profile(prof, key);
#endif
     loop:
	  if (xml) octstr_destroy(xml);	  	  
	  if (key) octstr_destroy(key);
     }
     closedir(dirp);

     return 0;
}


static MmsUaProfile *profile_fetch(Octstr *profile_url)
{
     Octstr *body = NULL;
     List *h, *rh = NULL;
     int status;
     MmsUaProfile *prof;

     gw_assert(profile_dict);
     
     debug("mms.uaprof", 0, "Entered fetcher");  

     if ((prof = dict_get(profile_dict, profile_url)) != NULL) 
	  return prof;

     h = http_create_empty_headers();
     http_header_add(h, "User-Agent", MM_NAME "/" MMSC_VERSION);	       
     
     status = mms_url_fetch_content(HTTP_METHOD_GET, profile_url, h, NULL, &rh, &body);   
     if (http_status_class(status) == HTTP_STATUS_SUCCESSFUL) {
	  prof = parse_uaprofile(body);
	  
	  debug("mms.uaprof", 0, "Fetcher got %s", octstr_get_cstr(profile_url));	  
	  if (prof) {
	       if (dict_put_once(profile_dict, profile_url, prof) != 1)
		    mms_warning(0, "mms_uaprof", NULL, "Duplicate ua profile fetched? (%s)?\n", 
			    octstr_get_cstr(profile_url));
	       else {
		    Octstr *fname;
		    FILE *f;
		    octstr_convert_range(profile_url, 0, octstr_len(profile_url), replace_slash);
		    fname = octstr_format("%.255s/%.254s", octstr_get_cstr(profile_dir), 
					  octstr_get_cstr(profile_url));
		    
		    f = fopen(octstr_get_cstr(fname), "w");
		    
		    if (f) {
			 octstr_print(f, body);
			 fclose(f);
		    } else 
			 mms_error(0, "mms_uaprof", NULL, "Failed to save profile data to cache file %s->%s\n",
			       octstr_get_cstr(fname), strerror(errno));
		    octstr_destroy(fname);
	       }
	  } else 
	       mms_error(0, "mms_uaprof", NULL, "Failed to parse UA prof url=%s\n", 
		     octstr_get_cstr(profile_url));	  
     }  else 
	  prof = NULL;     

     octstr_destroy(body);

     if (h) http_destroy_headers(h);
     if (rh) http_destroy_headers(rh);
     
     return prof;
}

static void init_format_table(void);
#define UACOUNT_PROFILE 1023
int mms_start_profile_engine(char *cache_dir)
{
     if (profile_dict)
	  return 0;

     profile_dir = octstr_create(cache_dir);
     
    if (!profile_dict)
	  profile_dict = dict_create(UACOUNT_PROFILE, 
				     (void (*)(void *))destroy_uaprof);     
     init_format_table();
     mms_load_ua_profile_cache(cache_dir); 
     
     return 0;
}

int mms_stop_profile_engine(void)
{     
     /* This will cause thread to stop and also destroy dict. */

     octstr_destroy(profile_dir);

     dict_destroy(profile_dict);

     profile_dict = NULL;
     return 0;
}

MmsUaProfile *mms_get_ua_profile(char *url)
{
     Octstr *s = octstr_create(url);
     MmsUaProfile *prof = NULL;

     gw_assert(profile_dict);
     prof = profile_fetch(s);
     octstr_destroy(s);
     return prof;
}

/* Content convertors. 
 * content types are listed in order of preference. 
 * Notes: We have a concept of an intermediate format. For audio it is WAV 
 * (of indeterminate frequency or sample size). Each type of command should expect input on stdin
 * and write output to standard out.
 * New 25-01-04: For images we now use Imagemagick to convert, hence no intermediate format. 
 */

#define NELEMS(a) (sizeof a/sizeof a[0])
struct {
     char *content_type; /* Content type. */
     unsigned long chash; /* hash value. */
     
     char *tostandard_cmd; /* Command to convert to standard format. */
     char *fromstandard_cmd; /* Command to convert from standard format. */
     
     char *file_ext; /* Standard file extension. */
     int multi_image; /* whether this format allows for multiple images in one file. */
     enum {TIMAGE=1,TAUDIO,TTEXT,TPRES,TOTHER} t;
} cformats[] = { 
  /* Note: Order of listing matters: 
   * For images, we prefer jpeg (smaller, better support),
   * For audio we prefer MP3 (smaller, well supported).
   */
     {"image/jpeg", 0, "jpegtopnm", "pnmtojpeg", "jpg", 0, TIMAGE},
     {"image/png", 0, "pngtopnm  ", "pnmtopng  ", "png", 0, TIMAGE},
     {"image/jpg", 0, "jpegtopnm", "pnmtojpeg", "jpg", 0, TIMAGE},
     {"image/tiff", 0, "tifftopnm", "pnmtotiff", "tiff", 1, TIMAGE},

     {"image/gif", 0, "giftopnm", "pnmquant 256 | ppmtogif", "gif", 1, TIMAGE},
     {"image/bmp", 0, "bmptopnm", "pnmquant 256 | ppmtobmp", "bmp", 1, TIMAGE},
     {"image/vnd.wap.wbmp", 0, "wbmptopbm", "ppmtopgm | pgmtopbm | pbmtowbmp", "wbmp", 0, TIMAGE},
#if 0
     {"image/x-bmp", 0, "bmptopnm", "pnmtobmp", "bmp", 0, TIMAGE},
     {"image/x-wmf", 0, "bmptopnm", "pnmtobmp", "bmp", 0, TIMAGE},
     {"image/vnd.wap.wpng", 0, "pngtopnm", "pngtobmp", "png", 0, TIMAGE},
     {"image/x-up-wpng", 0, "pngtopnm", "pnmtopng", "png", 0, TIMAGE},
#endif
     {"audio/mpeg", 0, "mpg123 -w - -", "lame - -", "mp3",0, TAUDIO},
     {"audio/wav", 0, "cat", "cat", "wav",0, TAUDIO},
     {"audio/x-wav", 0, "cat", "cat", "wav",0, TAUDIO},
     {"audio/basic", 0, "sox -t au -r 8000 -b -c 1 - -t wav -", 
      "sox -t wav - -t au -b -c 1 -r 8000 - lowpass 3700", "au",0, TAUDIO},
     {"audio/amr", 0, "amrdecoder - - | sox -t raw -s -w -c 1 -r 8000 - -t wav -", 
      "sox -t wav - -t raw -s -w -c 1 -r 8000 - lowpass 3700 | amrencoder MR122 - -", "amr",0, TAUDIO},
     {"audio/x-amr", 0, "amrdecoder - - | sox -t raw -s -w -c 1 -r 8000 - -t wav -", 
      "sox -t wav - -t raw -s -w -c 1 -r 8000 - lowpass 3700 | amrencoder MR122 - -", "amr",0, TAUDIO},
     {"audio/amr-wb", 0, "amrdecoder - - | sox -t raw -s -w -c 1 -r 8000 - -t wav -", 
      "sox -t wav - -t raw -s -w -c 1 -r 8000 - lowpass 3700 | amrencoder MR122 - -", "amr",0, TAUDIO},

     {"audio/au", 0,"sox -t au -s - -t wav -","lame - -","mp3",0,TAUDIO},
#if 0
     {"audio/midi", 0, "cat", "cat", "mid",0, TAUDIO},
     {"audio/sp-midi", 0, "cat", "cat", "mid",0, TAUDIO},
#endif
};

/* Image commands for finding resolution, scaling and conversion. */
#define IMGRESCMD "identify -format '%%w %%h' %s:%s"
#define IMGSCALECMD "convert -scale '%ldx%ld>' %s:%s %s:-"
#define IMGCONVERTCMD "convert '%s:%s' '%s:%s'"

static void init_format_table(void)
{
     int i;
     
     for (i = 0; i < NELEMS(cformats); i++)
	  cformats[i].chash = hash_key(octstr_imm(cformats[i].content_type));
     
}

/* Removes an object by making it text/plain. For now not configurable. */
static void remove_object(MIMEEntity *m, Octstr *ctype)
{
     List *h = mime_entity_headers(m);
     Octstr *s = octstr_format("Unsupported object (content type %S) removed", ctype);

     http_header_remove_all(h, "Content-Type");     
     http_header_add(h, "Content-Type", "text/plain");

     mime_replace_headers(m, h);
     http_destroy_headers(h);

     while (mime_entity_num_parts(m) > 0) /* Delete all parts, if any. */
	  mime_entity_remove_part(m, 0);
     mime_entity_set_body(m, s);
     octstr_destroy(s);
}

static void mktmpfname(char fname[])
{
     sprintf(fname, "%s/t%ld.%ld.%ld", 
#ifdef P_tmpdir
	     P_tmpdir,
#else
	     "/tmp"
#endif
	     random(), (long)getpid(), (long)time(NULL));     
}

static Octstr *mknewname(Octstr *oldname, char *ext)
{
     Octstr *s;

     int i = octstr_search_char(oldname, '.', 0);

     if (i<0)
	  return octstr_format("%S.%s", oldname, ext);

     s = octstr_copy(oldname, 0, i+1);
     octstr_append_cstr(s,ext);
     return s;
}

static void replace_ctype(List *headers, char *newcontent_type, List *params_h)
{
     Octstr *ct;
     if (gwlist_len(params_h) > 0) {
	  Octstr *tmp = make_value_parameters(params_h);
	  ct  = octstr_format("%s; %S", newcontent_type, tmp);
	  octstr_destroy(tmp);
     } else 
	  ct  = octstr_format("%s", newcontent_type);
     
     http_header_remove_all(headers, "Content-Type");	  
     http_header_add(headers, "Content-Type", octstr_get_cstr(ct));
     octstr_destroy(ct);
}

static void replace_body(MIMEEntity *msg, Octstr *newbody, List *params_h, 
			 char *newcontent_type, char *file_ext, 
			 int add_disposition_header)
{
     Octstr *part_name;
     Octstr *new_partname = NULL;
     List *h = mime_entity_headers(msg);

     mime_entity_set_body(msg, newbody); /* Replace the body. */

     if ((part_name = http_header_value(params_h, octstr_imm("name"))) != NULL) {
	  Octstr *tmp = mknewname(part_name, file_ext);
	  http_header_remove_all(params_h, "name");
	  http_header_add(params_h, "name", octstr_get_cstr(tmp));
	
	  octstr_destroy(part_name);
	  new_partname = tmp;
     }
     
     replace_ctype(h, newcontent_type, params_h);

     if (add_disposition_header) {
	  Octstr *tmp = octstr_format("inline; filename=\"%S\"",
				      new_partname ? new_partname : octstr_imm("any"));
	  http_header_add(h, "Content-Disposition", octstr_get_cstr(tmp));
	  octstr_destroy(tmp);
     }
     mime_replace_headers(msg,h);
     http_destroy_headers(h);
     octstr_destroy(new_partname);
}

/* Modify the message based on the user agent profile data. Return 1 if was supported, 0
 * otherwise 
 */
static int modify_msg(MIMEEntity *msg, MmsUaProfile *prof)
{
     int i, n, type, send_data;
     int j,m;
     Octstr *content_type = NULL, *params = NULL, *s;
     List *params_h;
     
     unsigned long chash;
     unsigned char supported = 0;
     
     int iindex, oindex;
     
     FILE *pf;
     char tmpf[40], tmpf2[40];
     Octstr *cmd = NULL;
     List *h = NULL;

     if (!msg) return 0;

     tmpf[0] = tmpf2[0] = 0; /* Clear .*/
     
     /* Get the content type, hash it. */  
     h = mime_entity_headers(msg);
     get_content_type(h, &content_type, &params);	       
     params_h = get_value_parameters(params);
     
#if 0
     debug("MMS uaprof:", 0, " content_type = ### %s & %s: Header dump follows:",  
	   octstr_get_cstr(content_type), params ? octstr_get_cstr(params) : "NULL");	  
     http_header_dump(params_h);
#endif
     
     if ((n = mime_entity_num_parts(msg)) > 0) {
	  Octstr *startp = http_header_value(params_h, octstr_imm("start"));
	  int sflag = 0;
	  
	  for (i = 0; i<n; i++) {
	       MIMEEntity *x = mime_entity_get_part(msg,i);
	       List *hx = mime_entity_headers(x);
 	       Octstr *cid = _x_get_content_id(hx);
	       int sup;
	       	       
	       debug("MMS uaprof: cid =###", 0, "%s", cid ? octstr_get_cstr(cid) : "NULL");
	       
	       sup = modify_msg(x, prof);
	       
	       if (!sup && /* not supported and is the presentation part, set flag */
		   cid && startp && octstr_compare(cid, startp) == 0) 
		    sflag = 1;		   
	       octstr_destroy(cid);

	       mime_entity_replace_part(msg, i, x); /* Put back changed one */
	       http_destroy_headers(hx);
	       mime_entity_destroy(x);
	  }
	  
	  /* If no start param but content type is other than multipart/mixed OR 
	   *   There is a start param but presentation type is not supported, 
	   * Or presentations are not supported.
	   */
	  if (sflag || 
	      (!startp && 
	       octstr_case_compare(content_type, octstr_imm("multipart/related")) == 0) || 
	      !(prof->ccppaccept.presentation || prof->ccppaccept.all)) {
	       /* MMS conformance guide says: If presentation part is removed or unsupported, 
		*  then change content type to multipart/mixed
		*/
	       
	       http_header_remove_all(params_h, "start"); 
	       http_header_remove_all(params_h, "type");
	       
	       replace_ctype(h, "multipart/mixed", params_h);
	  }
	  
	  octstr_destroy(startp);
	  
	  supported = 1;
	  goto done;
     }

     
     if (octstr_case_search(content_type, octstr_imm("image/"), 0) == 0)
	  type = TIMAGE;
     else if (octstr_case_search(content_type, octstr_imm("audio/"), 0) == 0)
	  type = TAUDIO;
     else if (octstr_case_search(content_type, octstr_imm("text/"), 0) == 0)
	  type = TTEXT;
     else
	  type = TOTHER;


     if (type == TTEXT) { /* Deal with charset issues. */
	  Octstr *charset = http_header_value(params_h, octstr_imm("charset")); 
	  char csupport = 0;
	  int i,n;
	  
	  if (charset == NULL || 
	      octstr_str_compare(charset, "unknown") == 0) {
	       octstr_destroy(charset);
	       charset = octstr_imm(DEFAULT_CHARSET);
	  }
	  
	  n = prof->ccppaccept.charset ? gwlist_len(prof->ccppaccept.charset) : 0;
	  
	  /* Is this character set supported? If so do nothing. */
	  for (i = 0; i<n; i++) 
	       if (octstr_case_compare(gwlist_get(prof->ccppaccept.charset,i), charset) == 0) {
		    csupport = 1;
		    break;
	       }

	  if (!csupport) 
	       for (i = 0; i<n; i++) {
		    Octstr *ncharset = gwlist_get(prof->ccppaccept.charset,i); /* Don't free this! */
		    Octstr *ct;
		    Octstr *s = mime_entity_body(msg);
		    if (charset_convert(s, octstr_get_cstr(charset), 
					octstr_get_cstr(ncharset)) != -1) { /* using libiconv...*/
			 Octstr *tmp;
			 
			 http_header_remove_all(params_h, "charset");
			 http_header_add(params_h, "charset", octstr_get_cstr(ncharset));
			 tmp = make_value_parameters(params_h);
			 
			 ct  = octstr_format("%S; %S", content_type, tmp);
			 octstr_destroy(tmp);
			 http_header_remove_all(h, "Content-Type");
			 http_header_add(h, "Content-Type", octstr_get_cstr(ct));
			 octstr_destroy(ct);
			 
			 mime_entity_set_body(msg,s); /* replace with new body. */
			 octstr_destroy(s);
			 break;  /* We succeeded in converting it so we shd go away. */
		    } else  
			 octstr_destroy(s);
	       }
	  
	  octstr_destroy(charset);
	  supported = 1;
	  goto done; /* No further processing for text/plain. */
     }

    /* find out if it is supported by the user agent. */
     chash = hash_key(content_type);
     if (prof->ccppaccept.all) /* Check if it accepts all content types. */
	  supported = 1;
     else 
	  for (i = 0, n = gwlist_len(prof->ccppaccept.content);
	       i<n; i++)
	       if ((unsigned long)gwlist_get(prof->ccppaccept._hash,i) == chash &&
		   octstr_case_compare(gwlist_get(prof->ccppaccept.content,i),content_type) == 0) {
		    supported = 1;
		    break;
	       }
     
     if (supported && type != TIMAGE)
	  goto done; /* If it is supported, go away now. 
		      * But for images we defer since we might have to  scale the image.
		      */
     else if (type == TOTHER) 
	  goto done; /* Not supported and not audio or image, will be removed at done. */

     
     /* At this point we have type = IMAGE (supported or not) OR type = AUDIO (unsuppported). */

     
     /* Find out if we have a means of converting this format to our internal format,
      * and if we have means of converting from internal to a format the UA supports.
      * For images this means: Can we read it? Can we write out a supported format?
      */

     iindex = -1;
     for (i = 0; i < NELEMS(cformats); i++)
	  if (cformats[i].chash == chash && 
	      octstr_case_compare(content_type, octstr_imm(cformats[i].content_type)) == 0) {
	       if (cformats[i].tostandard_cmd) 
		    iindex = i; /* Only if the cmd exists actually. */
	       break;
	  }

     oindex = -1;
     for (i = 0; i < NELEMS(cformats); i++)
	  if (cformats[i].fromstandard_cmd) /* Check only ones we can convert from. */
	       for (j = 0, m = gwlist_len(prof->ccppaccept.content); j<m; j++) 
		    if ((unsigned long)gwlist_get(prof->ccppaccept._hash,j) == cformats[i].chash &&
			cformats[i].t == type && /* Convert to like type ! */
			octstr_case_compare(gwlist_get(prof->ccppaccept.content,j),
					    octstr_imm(cformats[i].content_type)) == 0){
			 oindex = i;
			 i = NELEMS(cformats); /* So the other loop breaks too. */
			 break;
		    }
     
     
     if (iindex < 0 || oindex < 0)  /* We don't know how to convert this one fully, so... */
	  goto done2; /* go away, don't even replace headers. */
     
     
     /* Whatever we have (audio or image) we know how to convert it fully, so ... */
     mktmpfname(tmpf);          
     if (type == TIMAGE) {
	  FILE *pf;
	  long x = 640, y = 480;
	  Octstr *icmd, *s;
	  char *selector;
	  
	  mktmpfname(tmpf2);	  
	  pf = fopen(tmpf2, "w");
	  if (!pf) 
	       goto done;
	  
	  s = mime_entity_body(msg);
	  n = octstr_print(pf, s);
	  m = fclose(pf);

	  octstr_destroy(s);
	  if (n < 0 || m != 0)
	       goto done; /* error .*/
	  
	  /* Get the image dimensions, see if we need to modify it. */
	  icmd = octstr_format(IMGRESCMD, 
				 cformats[iindex].file_ext, tmpf2);
	  
	  pf = popen(octstr_get_cstr(icmd), "r");
	  octstr_destroy(icmd);
	  
	  if (!pf)
	       goto done;
	  fscanf(pf, "%ld %ld", &x, &y);
	  pclose(pf);

	  icmd = NULL; /* Reset to NULL. */
	  if (x > prof->maxres.x ||
	      y > prof->maxres.y)
	       icmd = octstr_format(IMGSCALECMD, prof->maxres.x, prof->maxres.y, 
		       cformats[iindex].file_ext,
		       tmpf2,
		       cformats[iindex].file_ext);
	  else if (supported) /* A supported image format and no need to scale, so go away. */
	       goto done;
	  
	  /* If the first image is multi but the second isn't, then add selector. */
	  if (cformats[iindex].multi_image &&  
	      !cformats[oindex].multi_image)
	       selector = "-[0]";
	  else
	       selector = "-";
	  /* We have an unsupported image now, or a supported one that must be scaled. 
	   * time to convert it. 
	   */
	  if (icmd) {
	       cmd = (supported) ? octstr_format("%S > %s", icmd, tmpf) : 
		    octstr_format("%S | " IMGCONVERTCMD,
				  icmd, cformats[iindex].file_ext, selector,
				  cformats[oindex].file_ext, tmpf);
	       octstr_destroy(icmd);
	  } else /* Image format MUST be unsupported */
	       cmd = octstr_format("cat %s | " IMGCONVERTCMD,
				   tmpf2, cformats[iindex].file_ext, selector,
				   cformats[oindex].file_ext, tmpf);

	  send_data = 0;
     } else  {/* Type is audio. */	  
	 cmd = octstr_format("%s | %s > %s", 
		  cformats[iindex].tostandard_cmd, 
		  cformats[oindex].fromstandard_cmd, tmpf);
	  send_data = 1;
     }

     pf = popen(octstr_get_cstr(cmd), "w");

     if (!pf)
	  goto done;
     
     if (send_data) { /* If this is not set then write the content... */
	  Octstr *sx = mime_entity_body(msg);
	  n = octstr_print(pf, sx);	  
	  octstr_destroy(sx);
     } else 
	  n = 0;
     m = pclose(pf);
     if (n < 0 || m != 0)
	  goto done; /* Error -- finish up. */

     if ((s = octstr_read_file(tmpf)) != NULL) {
	  replace_body(msg, s, params_h, 
		       supported == 1 ? cformats[iindex].content_type : cformats[oindex].content_type,
                       supported == 1 ? cformats[iindex].file_ext : cformats[oindex].file_ext,0);
	  octstr_destroy(s);
	  supported = 1;
	  goto done2; /* we are done, don't even change headers. */
     }  else /* failed to convert, hence unsupported. */
	  goto done;
     
 done:
     if (h) 
	  mime_replace_headers(msg,h);  
 done2:
     if (!supported)
	  remove_object(msg, content_type);

     if (h)
	  http_destroy_headers(h);
     
     octstr_destroy(content_type);     
     octstr_destroy(params);
     if (params_h)
	  http_destroy_headers(params_h);
     if (tmpf[0])
	  unlink(tmpf);

     if (tmpf2[0])
	  unlink(tmpf2);
     
     octstr_destroy(cmd);
     return supported;
}


int mms_transform_msg(MmsMsg *inmsg, MmsUaProfile *prof, MmsMsg **outmsg)
{
     MIMEEntity *m;
     Octstr *s;
     
     if (!prof)
	  return -1;
     else if (!prof->ccppaccept.content)
	  return -2;

     m = mms_tomime(inmsg,0);
 
     modify_msg(m, prof);

     *outmsg = mms_frommime(m);

     mime_entity_destroy(m);

     s = mms_tobinary(*outmsg);

     if (octstr_len(s) > prof->maxmsgsize) {
	  mms_destroy(*outmsg);
	  *outmsg = NULL;
     }

     octstr_destroy(s);

     /* Don't free profile! It is cached. */
     return 0;
}

/* XXX Needs some work: Ensure we don't mod  SMIL other than that specified in the start param. 
 * Also may be we should only mod the SMIL if content type of entity is multipart/related
 * Also if the start param is missing we should perhaps stick it in.
 */

static int format_special(MIMEEntity *m, 
			  int trans_smil, char *txtmsg, char *htmlmsg, int *counter)
{
     int type;
     Octstr *content_type = NULL, *params = NULL, *s;
     int i, n;
     unsigned long chash;
     FILE *pf;
     char tmpf[40];
     Octstr *cmd = NULL;
     List *params_h;
     List *headers;
     
     tmpf[0] = 0;
     headers = mime_entity_headers(m);
     get_content_type(headers, &content_type, &params);	       
     params_h = get_value_parameters(params);

     if ((n = mime_entity_num_parts(m)) > 0) {
	  int presindex = -1;
	  Octstr *presbody = NULL;
	  Octstr *start = http_header_value(params_h, octstr_imm("start"));
	  
	  for (i = 0; i<n; i++) {/* format sub-parts, find presentation part too */
	       MIMEEntity *x = mime_entity_get_part(m,i);
	       Octstr *ctype = NULL, *charset = NULL;
	       List *hx = mime_entity_headers(x);
	       Octstr *cid = _x_get_content_id(hx);
	       
	       http_header_get_content_type(hx, &ctype, &charset);		    
	       
	       /* Find presentation part: If we have start param, and it matches
		* this one, and this one is SMIL, then...
		*/
	       if (start && cid && octstr_compare(cid, start) == 0 && 
		   octstr_case_compare(ctype, octstr_imm(PRES_TYPE)) == 0) 
		    presindex = i;
	      

	       octstr_destroy(ctype);
	       octstr_destroy(charset);	       
	       octstr_destroy(cid);

	       format_special(x, trans_smil, txtmsg, htmlmsg, counter);
	       mime_entity_replace_part(m, i, x);

	       http_destroy_headers(hx);
	       mime_entity_destroy(x);
	  }
	  
	  octstr_destroy(start);

	  if (trans_smil && presindex >= 0) { /* Reformat. */
	       MIMEEntity *x, *pres;
	       Octstr *btxt = octstr_create("");
	       List *h = NULL, *h2 = NULL;
	       Octstr *tmp;
	       
	       /* Remove type & start param from top level. */
	       http_header_remove_all(params_h, "type");	       
	       replace_ctype(headers, "multipart/related", params_h);
	       	       
	       /* Put content ids on all siblings, and build html. */
	       for (i = 0, n = mime_entity_num_parts(m); i<n; i++) {
		    MIMEEntity *x = mime_entity_get_part(m,i);
		    List *hx = NULL;
		    Octstr *cid, *pname;
		    Octstr *ctype = NULL, *cparams = NULL;
		    Octstr *y, *loc, *cidurl;
		    List *cparamsl;
		    
		    if (i == presindex) goto loop; /* Skip the presentation param. */

		    hx = mime_entity_headers(x);
		    cid = _x_get_content_id(hx);

		    if (cid == NULL) {
			 time_t t = time(NULL);
			 char ch[2];

			 ch[0] = ((t%2) ? 'A' : 'a') + (t%25);  /* Make them a bit more unique. */
			 ch[1] = 0; 
			 
			 cid = octstr_format("<item-%s-%d-%d>", ch, *counter, (t%99989));
			 http_header_add(hx, "Content-ID", octstr_get_cstr(cid));

			 ++*counter;
		    } else if (octstr_get_char(cid, 0) != '<') { /* fix up for badly behaved clients. */
			 octstr_insert_char(cid, 0, '<');
			 octstr_append_char(cid, '>');
			 http_header_remove_all(hx, "Content-ID");
			 http_header_add(hx, "Content-ID", octstr_get_cstr(cid));
		    }

		    y = octstr_copy(cid, 1, octstr_len(cid) - 2);
		    loc = http_header_value(hx, octstr_imm("Content-Location"));

		    cidurl = octstr_duplicate(y);
		    octstr_url_encode(cidurl);
		    		    
		    get_content_type(hx, &ctype, &cparams); /* Get type of object. */
		    
		    if (cparams) { /* Get its name. */
			 if ((cparamsl = get_value_parameters(cparams)) != NULL) {		    
			      pname = http_header_value(cparamsl, octstr_imm("name"));
			      http_destroy_headers(cparamsl);
			 }  else
			      pname = octstr_duplicate(loc ? loc : y);
			 octstr_destroy(cparams);
		    } else 
			 pname = octstr_imm("unknown");

		    if (octstr_case_search(ctype, octstr_imm("image/"), 0) == 0) 
			 octstr_format_append(btxt, 
					      "\n<img src=\"cid:%S\" border=0 alt=\"%S\"><br><br>\n",
					      cidurl, pname ? pname : octstr_imm("image"));
		    else if (octstr_case_search(ctype, octstr_imm("text/"), 0) == 0) {
			 Octstr *s = mime_entity_body(x);
			 octstr_format_append(btxt, "%S<br><br>\n", s);
			 octstr_destroy(s);
		    }
#if 0
		    else if (octstr_case_search(ctype, octstr_imm("audio/"), 0) == 0) 
			 octstr_format_append(btxt, 
					      "<embed  src=\"cid:%S\" >%S</embed><br><br>\n",
				      cidurl, ctype, pname);		  		    
#endif
		    else
			 octstr_format_append(btxt, 
					      "<a type=\"%S\" href=\"cid:%S\">%S</a><br><br>\n",
					      ctype, cidurl, pname ? pname : octstr_imm(""));		  
		    
		    http_header_remove_all(hx, "Content-Location");
		    
		    mime_replace_headers(x, hx); /* put back headers, replace part in main...*/
		    mime_entity_replace_part(m, i, x);
		    
		    http_destroy_headers(hx);
		    
		    octstr_destroy(y);
		    octstr_destroy(loc);
		    octstr_destroy(ctype);
		    octstr_destroy(cidurl);
		    octstr_destroy(pname);
			 
		    octstr_destroy(cid);
		    
	       loop:
		    mime_entity_destroy(x);
	       }

	       pres = mime_entity_get_part(m,presindex);
	       presbody = mime_entity_body(pres);
	       h = mime_entity_headers(pres);
	       
	       http_header_remove_all(h, "Content-Type");
	       http_header_add(h, "Content-Type", "multipart/alternative");
	       
	       mime_replace_headers(pres,h);
	       http_destroy_headers(h);

	       /*  first the text part ... */
	       x = mime_entity_create();
	       h2 = http_create_empty_headers();
	       http_header_add(h2, "Content-Type", "text/plain");
	       tmp = octstr_create(txtmsg ? txtmsg : "");

	       mime_replace_headers(x, h2);
	       mime_entity_set_body(x,tmp);

	       mime_entity_add_part(pres, x);

	       http_destroy_headers(h2);
	       octstr_destroy(tmp);
	       mime_entity_destroy(x);
	       
	       /* Lets also leave the pres part in there, just in case somebody knows how to handle it... */
	       x = mime_entity_create();
	       h2 = http_create_empty_headers();
	       http_header_add(h2, "Content-Type", PRES_TYPE);
	       mime_replace_headers(x, h2);
	       mime_entity_set_body(x, presbody);
	       mime_entity_add_part(pres, x);
	       
	       http_destroy_headers(h2);
	       mime_entity_destroy(x);

	       	      
	       /* then the html part. */
	       x = mime_entity_create();
	       h2 = http_create_empty_headers();
	       http_header_add(h2, "Content-Type", "text/html");	  
	       tmp = octstr_format("<html><body><center>%s<hr><br>%S<hr></center></body></html>\n",
				       htmlmsg ? htmlmsg : "", btxt);
	       
	       mime_replace_headers(x, h2);
	       mime_entity_set_body(x,tmp);

	       mime_entity_add_part(pres, x);

	       http_destroy_headers(h2);
	       octstr_destroy(tmp);
	       mime_entity_destroy(x);


	       mime_entity_replace_part(m, presindex, pres);
	       mime_entity_destroy(pres);

	       octstr_destroy(presbody);
	       octstr_destroy(btxt);	       	      
	  }
	  goto done;
     }
	
     
     if (octstr_case_search(content_type, octstr_imm("image/"), 0) == 0)
	  type = TIMAGE;
     else if (octstr_case_search(content_type, octstr_imm("audio/"), 0) == 0)
	  type = TAUDIO;
     else if (octstr_case_search(content_type, octstr_imm("text/"), 0) == 0)
	  type = TTEXT;
     else
	  type = TOTHER;
     
     chash = hash_key(content_type);
     
     if (type == TAUDIO) { /* Find the preferred format (audio only, leave images alone!), 
			    * assuming we can support this one.
			    */
	  int ipref = -1, iindex = -1, o;
	  
	  for (i = 0; i < NELEMS(cformats); i++) {
	       if (ipref < 0 && cformats[i].t == type && 
		   cformats[i].fromstandard_cmd)
		    ipref = i;
	       
	       if (cformats[i].chash == chash && 
		   octstr_case_compare(content_type, 
				       octstr_imm(cformats[i].content_type)) == 0) {
		    if (cformats[i].tostandard_cmd) 
			 iindex = i; /* Only if the cmd exists actually. */
		    break;
	       }
	  }

	  if (iindex < 0 || ipref < 0) /* Can't change it.... */
	       goto done; 
	  
	  mktmpfname(tmpf);	  
	  if (type == TAUDIO)
	       cmd = octstr_format("%s | %s > %s", 
				   cformats[iindex].tostandard_cmd, 
				   cformats[ipref].fromstandard_cmd, tmpf);
	  else 
	       cmd = octstr_format(IMGCONVERTCMD,
				   cformats[iindex].file_ext, "-",
				   cformats[ipref].file_ext, tmpf);

	  pf = popen(octstr_get_cstr(cmd), "w");
	  
	  if (!pf)
	       goto done;
	  s = mime_entity_body(m);
	  n = octstr_print(pf, s);	  
	  octstr_destroy(s);

	  o = pclose(pf);
	  
	  if (n < 0 || o != 0)
	       goto done; /* Error -- finish up. */
	  
	  if ((s = octstr_read_file(tmpf)) != NULL) {
	       replace_body(m, s, params_h, 
			    cformats[ipref].content_type, 
			    cformats[ipref].file_ext,1);
	       octstr_destroy(s);
	  } else 
	       goto done;

     } else if (type == TTEXT) {/* change to default charset. */
	  Octstr *charset = http_header_value(params_h, octstr_imm("charset")); 
	  Octstr *s;
	  if (charset == NULL || 
	      octstr_case_compare(charset, octstr_imm("unknown")) == 0 || 
	       octstr_case_compare(charset, octstr_imm(DEFAULT_CHARSET)) == 0) {
	       if (charset) octstr_destroy(charset);
	       goto done; /* Nothing more to do here. */
	  }
	  
	  s = mime_entity_body(m);
	  if (charset_convert(s, octstr_get_cstr(charset), 
			      DEFAULT_CHARSET) != -1) { /* using libiconv...*/
	       Octstr *tmp, *ct;
	     
	       http_header_remove_all(params_h, "charset");
	       http_header_add(params_h, "charset", DEFAULT_CHARSET);
	       tmp = make_value_parameters(params_h);
	       
	       ct  = octstr_format("%S; %S", content_type, tmp);
	       octstr_destroy(tmp);
	       http_header_remove_all(headers, "Content-Type");
	       http_header_add(headers, "Content-Type", octstr_get_cstr(ct));
	       octstr_destroy(ct);
	       mime_entity_set_body(m, s);
	  } /* Else goto done. */
	  octstr_destroy(s);
     }  /* Else do nothing. */
     
 done:

     if (headers) {
	  mime_replace_headers(m, headers);
	  http_destroy_headers(headers);
     }
     
     octstr_destroy(content_type);
     octstr_destroy(params);
     if (params_h)
	  http_destroy_headers(params_h);
     octstr_destroy(cmd);

     if (tmpf[0])
	  unlink(tmpf);

     return 0;
}

int mms_format_special(MmsMsg *inmsg,
			       int trans_smil, 
			       char *txtmsg, 
			      char *htmlmsg, MIMEEntity **outmsg)
{
     MIMEEntity *m = mms_tomime(inmsg,0);
     int ct = 0;

     if (!m)
	  return -1;

     format_special(m, trans_smil, txtmsg, htmlmsg, &ct);

     *outmsg = m;
     return 0;
}

extern unsigned long mms_ua_maxmsgsize(MmsUaProfile *prof)
{  
     return prof ? prof->maxmsgsize : 0;
}

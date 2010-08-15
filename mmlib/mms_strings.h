/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMS strings module
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMS_STRINGS_DEFINED__
#define __MMS_STRINGS_DEFINED__

typedef enum {
  MS_1_1 = 1,
  MS_1_2 = 2,
  MM7_5 = 5
} mms_encoding;
#define MMS_DEFAULT_VERSION "1.0"
/* Declare the functions */
#define LINEAR(name, strings) \
Octstr *mms_##name##_to_string(long number); \
unsigned char *mms_##name##_to_cstr(long number); \
long mms_string_to_##name(Octstr *ostr); \
long mms_string_to_versioned_##name(Octstr *ostr, int version); 
#define STRING(string)
#include "mms_strings.def"


#define LINEAR(name,strings)
#define STRING(string)
#define NAMED(name, strings) enum mms_##name##_enum { strings mms_##name##_dummy };
#define NSTRING(string, name) name,
#define VNSTRING(version, string, name) name,
#include "mms_strings.def"

#include "wap/wsp_strings.h"

void mms_strings_init(void);
void mms_strings_shutdown(void);
#endif

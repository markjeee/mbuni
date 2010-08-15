/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 *  config file functions (when conf is loaded from another source)
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */ 
#ifndef __MMS_CFG_IMPL__INCLUDED__
#define __MMS_CFG_IMPL__INCLUDED__
#include "mms_cfg.h"

typedef struct mCfgImpl mCfgImpl; /* Implementation-specific struct for config info. */

/* each implementation must export struct, symbol 'cfg_funcs'. */
typedef  struct mCfgImpFuncs {
     mCfgImpl *(*read)(Octstr *init_param);
     void (*destroy)(mCfgImpl *);
     mCfgGrp *(*cfg_get_single)(mCfgImpl *cfg, Octstr *name);
     List *(*get_multi)(mCfgImpl *cfg,Octstr *name); /* List of mCfgGrp */
     mCfgGrp *(*get_multi_by_field)(mCfgImpl *cfg, Octstr *name, Octstr *field, Octstr *value);
     Octstr *(*get)(mCfgImpl *cfg, mCfgGrp *grp, Octstr *name);
     Octstr *(*get_grp_name)(mCfgImpl *, mCfgGrp *);
     void (*destroy_grp)(mCfgImpl *cfg, mCfgGrp *);
} mCfgImpFuncs;


#endif

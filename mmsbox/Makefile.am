libmms = $(top_builddir)/mmlib/libmms.a

bin_PROGRAMS = mmsbox
mmsbox_SOURCES = mmsbox.c mmsbox_cdr.c mmsbox_cfg.c dlr.c bearerbox.c mmsbox_resolve.c mmsbox_resolve_shell.c
mmsbox_LDADD = $(libmms)

EXTRA_DIST = mmsbox_cdr.h mmsbox_cfg.h  mmsbox.h mmsbox_mt_filter.h mmsbox_resolve_shell.h mmsbox_resolve.h mmsbox_mmsc.h

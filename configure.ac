dnl  Mbuni - Open  Source MMS Gateway 
dnl
dnl Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
dnl
dnl Paul Bagyenda <bagyenda@dsmagic.com>
dnl 
dnl This program is free software, distributed under the terms of
dnl the GNU General Public License, with a few exceptions granted (see LICENSE)
dnl
AC_PREREQ(2.61)

AC_INIT([mbuni],[cvs],[devel@mbuni.org])

dnl Set version

AC_MSG_CHECKING([Retrieving Mbuni version])
M_VERSION=`head -n 1 VERSION`
if test "x$M_VERSION" = "xcvs"; then
        CVS_DATE=`head -1 ChangeLog | cut -f 1 -d ' ' | sed 's/-//g'`
        M_VERSION="$M_VERSION-$CVS_DATE"
fi

AC_MSG_RESULT([$M_VERSION])

AC_CONFIG_SRCDIR([mmlib/mms_util.c])
AC_CONFIG_AUX_DIR(autotools)
AM_INIT_AUTOMAKE([mbuni],[$M_VERSION])
AC_CONFIG_HEADERS([mbuni-config.h])
AM_MAINTAINER_MODE
AC_CANONICAL_HOST

dnl Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL
AC_PROG_RANLIB

dnl add mmlib to the include path
INCLUDES='-I$(top_srcdir)/mmlib -I$(top_builddir)/mmsc -I$(top_srcdir)/mmsc -I$(top_builddir)/mmsbox -I$(top_srcdir)/mmlib -I$(top_builddir)/mmsbox'
AC_SUBST([INCLUDES])


dnl Checks for libraries.
AC_CHECK_LIB([dl], [dlsym])
dnl AC_CHECK_LIB([iconv], [libiconv], [LIBS="$LIBS -liconv"])
AC_CHECK_LIB([pthread], [pthread_create])
dnl AC_CHECK_LIB([xml2], [xmlStrcasecmp], [],
dnl 	AC_MSG_ERROR([LibXML2 is required]))
AC_CHECK_FUNCS([backtrace])

dnl Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([float.h memory.h stdlib.h string.h sys/file.h unistd.h])

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_STRUCT_TM
AC_C_VOLATILE

dnl Checks for library functions.
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_ERROR_AT_LINE
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_TYPE_SIGNAL
AC_FUNC_STAT
AC_CHECK_FUNCS([floor localtime_r memset sqrt strerror strrchr strtol])

dnl Change a few things (a la kannel config)
EXE_EXT=""
LIB_EXT="a"
case "$host" in
  *-sun-solaris* | *SunOS* | *-pc-solaris*) 
    CFLAGS="$CFLAGS -DSunOS=1 -D_POSIX_PTHREAD_SEMANTICS -D_REENTRANT"
    ;;
  *-cygwin*)
    EXE_EXT=".exe"
    ;;
  *apple-darwin*) 
    CFLAGS="$CFLAGS -DDARWIN=1 -O4 -Wall" 
    LIB_EXT="dylib"
    ;;
  *-linux*)
    CFLAGS="$CFLAGS -D_XOPEN_SOURCE=600 -D_BSD_SOURCE -O4 -Wall"
    LDFLAGS="$LDFLAGS -rdynamic"
    ;;
  *-*-openbsd* | *-*-freebsd*)
    CFLAGS="$CFLAGS -pthread"
        AC_CHECK_LIB(c_r, pthread_exit, [LIBS="$LIBS -lc_r -lpthread"; pthread="yes"])
    ;;
esac

AC_ARG_WITH(cflags,
[  --with-cflags=FLAGS     use FLAGS for CFLAGS],
CFLAGS="$CFLAGS $withval")

AC_ARG_WITH(libs,
[  --with-libs=FLAGS       use FLAGS for extra libraries],
LIBS="$LIBS $withval")

dnl Check for large file handling
AC_SYS_LARGEFILE(CFLAGS)
if test "${ac_cv_sys_file_offset_bits}" != no ; then
    CFLAGS="$CFLAGS -D_FILE_OFFSET_BITS=${ac_cv_sys_file_offset_bits}"
fi
if test "${ac_cv_sys_large_files}" != "no" ; then
    CFLAGS="$CFLAGS -D_LARGE_FILES=${ac_cv_sys_large_files}"
fi

dnl implement SSL stuff.

dnl Implement the --with-ssl option.


AC_ARG_WITH(ssl,
[  --with-ssl[=DIR]          where to look for OpenSSL libs and header files
                          DIR points to the installation [/usr/local/ssl]],
[ if test -d "$withval"; then
    ssllib="$withval/lib";
    sslinc="$withval/include"
  else
    AC_MSG_ERROR(Unable to find OpenSSL libs and/or directories at $withval)
  fi
])

dnl Implement --enable-ssl option.

AC_MSG_CHECKING([whether to compile with SSL support])
AC_ARG_ENABLE(ssl,
[  --enable-ssl            enable SSL client and server support [enabled]], [
  if test "$enableval" = no ; then
    AC_MSG_RESULT(disabled)
    ssl=no
  else
    ssl=yes
  fi
],[
  ssl=yes
])

if test "$ssl" = "yes" ; then
  dnl test only if --with-ssl has not been used
  if test "x$ssllib" = "x" && test "x$sslinc" = "x"; then
    for loc in /usr/lib /usr/local/ssl/lib /usr/local/openssl/lib; do
      if test -f "$loc/libssl.$LIB_EXT"; then
        ssllib="$loc"
      fi
    done
    for loc in /usr/include/ssl /usr/include/openssl /usr/local/ssl/include \
               /usr/local/openssl/include; do
      if test -d "$loc"; then
        sslinc="$loc"
      fi
    done
  fi
  AC_MSG_RESULT(trying $ssllib $sslinc)
fi

dnl Implement the SSL library checking routine.
dnl This will define HAVE_LIBSSL in config.h

if test "x$ssllib" != "x" && test "x$sslinc" != "x"; then
  CFLAGS="$CFLAGS -I$sslinc"
  LIBS="$LIBS -L$ssllib"

  AC_PATH_PROG(OPENSSL, openssl, no)
  if test "$OPENSSL" = "yes"; then
    AC_MSG_CHECKING([openssl version])
    openssl_version=`$OPENSSL version | awk '{print $2}'`
    AC_MSG_RESULT([$openssl_version])
  fi
  AC_CHECK_LIB(crypto, CRYPTO_lock,
  [ LIBS="$LIBS -lcrypto"
    AC_CHECK_LIB(ssl, SSL_library_init,
    [ AC_CHECK_LIB(ssl, SSL_connect)
      AC_CHECK_HEADERS(openssl/x509.h openssl/rsa.h openssl/crypto.h \
                       openssl/pem.h openssl/ssl.h openssl/err.h)
      AC_MSG_CHECKING(whether the OpenSSL library is multithread-enabled)
      AC_TRY_RUN([
        #define OPENSSL_THREAD_DEFINES
        #include <openssl/opensslconf.h>
        int main(void) {
        #if defined(THREADS)
          exit(0);
        #elif defined(OPENSSL_THREADS)
          exit(0);
        #else
          exit(1);
        #endif
        }
      ], [
        AC_MSG_RESULT(yes)
        AC_DEFINE(HAVE_LIBSSL)
        LIBS="$LIBS -lssl"
        AC_MSG_CHECKING([whether to compile with SSL support])
        AC_MSG_RESULT(yes)
      ], [
        AC_ARG_ENABLE(ssl-thread-test,
        [  --disable-ssl-thread-test  disable the multithread test for the OpenSSL library
                          this will force to continue even if the test fails],
        [ if test "$enableval" = no ; then
            AC_MSG_RESULT([no, continue forced])
          fi
        ], [
          AC_MSG_RESULT(no)
          AC_MSG_ERROR(Either get a multithread-enabled SSL or configure with --disable-ssl)
        ])
      ], echo "Cross-compiling; make sure your SSL library is multithread-enabled"
      )
    ])
  ])
fi

dnl Need to check for kannel and also that kannel has been patched.

dnl Implement the --with-kannel-dir option
AC_ARG_WITH(kannel-dir,
[  --with-kannel-dir=DIR    where to look for Kannel Gateway libs and header files
                                                DIR points to the installation [/usr/local] ]
,
[
gwloc=""
if test -d "$withval" ; then
        gwloc="$withval"
fi
])

AC_PATH_PROG(GW_CONFIG, gw-config, no, [$gwloc/bin:$gwloc:$PATH])
dnl check for Kannel gw-config
if test "$GW_CONFIG" = "no"; then
found=""
        for loc in $pgsqlloc /usr /usr/local ; do
                if test "x$found" = "x" ; then
                        AC_MSG_CHECKING([for Kannel include files in])
                        AC_MSG_RESULT($loc)
                        AC_CHECK_FILE("$loc/include/kannel/gw-config.h",
                                [CFLAGS="$CFLAGS -I$loc/include/kannel"; 
				LDFLAGS="$LDFLAGS -L$loc/lib/kannel -lwap -lgwlib";
				found=1
                                ])
                fi
        done;
        if test "x$found" != "x1" ; then
                AC_MSG_ERROR([Unable to find gw-config.h, please provide a --with-kannel-dir=
<dir> location])
        fi
else
        dnl gw_config found
        AC_MSG_CHECKING([Kannel version])
        gw_version=`$GW_CONFIG --version`
        AC_MSG_RESULT([$gw_version])
        AC_MSG_CHECKING([Kannel libs])
        if ! $GW_CONFIG --libs &>/dev/null ; then
                gw_libdir=`$GW_CONFIG --libs`
                LDFLAGS="$LDFLAGS $gw_libdir"
		LIBS="$LIBS $gw_libdir"
                AC_MSG_RESULT([$gw_libdir])
        fi
        AC_MSG_CHECKING([Kannel includes])
        if ! $GW_CONFIG --cflags &>/dev/null ; then
                CFLAGS="$CFLAGS `$GW_CONFIG --cflags`"
                gw_incdir=`$GW_CONFIG --cflags`
                AC_MSG_RESULT([$gw_incdir])
        fi
fi

AC_CHECK_LIB([gwlib], [cfg_create], [], AC_MSG_ERROR([Kannel gwlib is required!]))
AC_CHECK_LIB([wap], [wsp_headers_pack], [], AC_MSG_ERROR([Kannel WAP lib is required!]))

dnl Implement the --with-pgsql-dir option.
pgsqlloc="/usr/local/pgsql"
AC_ARG_WITH(pgsql-dir,
[  --with-pgsql-dir=DIR    where to look for PostgreSQL libs and header files
						DIR points to the installation [/usr/local/pgsql] ],
[
pgsqlloc=""
if test -d "$withval" ; then
	pgsqlloc="$withval"
fi
])

AC_PATH_PROG(PGSQL_CONFIG, pg_config, no, [$pgsqlloc/bin:$pgsqlloc:/usr/lib/postgresql/bin:${PATH}])
dnl check for PgSQL >= 7.2 style pg_config information
if test "$PGSQL_CONFIG" = "no"; then
found=""
	for loc in $pgsqlloc /usr /usr/local ; do
		if test "x$found" = "x" ; then
			AC_MSG_CHECKING([for PostgresSQL include files in])
			AC_MSG_RESULT($loc)
			AC_CHECK_FILE("$loc/include/postgresql/libpq-fe.h",
				[CFLAGS="$CFLAGS -I$loc/include/postgresql"; LIBS="-L$loc/lib/postgresql -lpq $LIBS"]; found=1,
				[AC_CHECK_FILE("$loc/include/pgsql/libpq-fe.h",
				[CFLAGS="$CFLAGS -I$loc/include/pgsql"; LIBS=" -L$loc/lib/pgsql $LIBS -lpq"]; found=1,
				[AC_CHECK_FILE("$loc/pgsql/include/libpq-fe.h",
					[CFLAGS="-I$loc/pgsql/include $CFLAGS"; LIBS="-L$loc/pgsql/lib $LIBS -lpq"]; found=1,
				)]
				)]
			)
		fi
	done

else
	dnl pg_config found
	AC_MSG_CHECKING([PostgreSQL version])
	pgsql_version=`$PGSQL_CONFIG --version`
	AC_MSG_RESULT([$pgsql_version])
	AC_MSG_CHECKING([PostgreSQL libs])
	if $PGSQL_CONFIG --libdir &>/dev/null ; then
		LIBS="-L`$PGSQL_CONFIG --libdir`  $LIBS -lpq "
		pg_libdir="`$PGSQL_CONFIG --libdir`"
		AC_MSG_RESULT([$pg_libdir])
	fi
	AC_MSG_CHECKING([PostgreSQL includes])
	if $PGSQL_CONFIG --includedir &>/dev/null ; then
		CFLAGS="-I`$PGSQL_CONFIG --includedir` $CFLAGS"
		pg_incdir=`$PGSQL_CONFIG --includedir`
		AC_MSG_RESULT([$pg_incdir])
	fi
fi
# set HAVE_LIBpq if we have libpq
AC_CHECK_LIB([pq], [PQexec], [has_pgsql=true])

AM_CONDITIONAL([BUILD_PGSQLQ], [test  x$has_pgsql = xtrue])

# Look for curl lib
AC_PATH_PROG(CURL_CONFIG, curl-config, no, [${PATH}])
if test "$CURL_CONFIG" != "no"; then
	dnl curl-config found
	AC_MSG_CHECKING([curl lib version])
	curl_version=`$CURL_CONFIG --version`
	AC_MSG_RESULT([$curl_version])
	AC_MSG_CHECKING([curl libs])
	if $CURL_CONFIG --libs &>/dev/null ; then
		curl_libs=`$CURL_CONFIG --libs`
		AC_MSG_RESULT([$curl_libs])
		LIBS="$LIBS $curl_libs"
	fi
	AC_MSG_CHECKING([curl includes])
	if $CURL_CONFIG --cflags &>/dev/null ; then
		curl_cflags=`$CURL_CONFIG --cflags`
		CFLAGS="$curl_cflags $CFLAGS"
		AC_MSG_RESULT([$curl_cflags])
	fi
fi

AC_CHECK_LIB([curl], [curl_global_init], [has_libcurl=true])

AM_CONDITIONAL([BUILD_MMSBOXMM1], [test  x$has_libcurl = xtrue])

dnl Add mbuni version to Cflags
# CFLAGS="$CFLAGS -DMBUNI_VERSION=\"$M_VERSION\""
AC_DEFINE_UNQUOTED(MBUNI_VERSION, "$M_VERSION", "Mbuni Version")

dnl May be we need to check for media conversion tools (imagemagick, etc)? 


AC_CONFIG_FILES([Makefile autotools/Makefile doc/Makefile doc/examples/Makefile doc/images/Makefile mmlib/Makefile mmsc/Makefile mmsbox/Makefile extras/Makefile extras/pgsql-queue/Makefile extras/mmsbox-mm1/Makefile])

AC_OUTPUT

cat<<X
License: 
  Mbuni (version $M_VERSION) - Open  Source MMS Gateway  - http://www.mbuni.org/

 Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com

 This program is free software, distributed under the terms of
 the GNU General Public License, with a few exceptions granted (see LICENSE)

 If you do not agree with the terms of the license, you must abort the 
 installation process now.
X

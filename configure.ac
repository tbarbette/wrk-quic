AC_PREREQ([2.69])
AC_INIT([WRKMP], 1.0)

#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_CONFIG_SRCDIR([src/wrk.c])
AC_CONFIG_HEADERS([src/config.h:config.h.in])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S

# Checks for libraries.
# FIXME: Replace `main' with a function in `-lcrypto':
AC_CHECK_LIB([crypto], [main])
# FIXME: Replace `main' with a function in `-ldl':
AC_CHECK_LIB([dl], [main])
# FIXME: Replace `main' with a function in `-lluajit':
AC_CHECK_LIB([luajit], [main])
# FIXME: Replace `main' with a function in `-lm':
AC_CHECK_LIB([m], [main])
# FIXME: Replace `main' with a function in `-lpthread':
AC_CHECK_LIB([pthread], [main])
# FIXME: Replace `main' with a function in `-lsocket':
AC_CHECK_LIB([socket], [main])
# FIXME: Replace `main' with a function in `-lssl':
AC_CHECK_LIB([ssl], [main])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h inttypes.h limits.h locale.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h strings.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_INT8_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T
AC_CHECK_TYPES([ptrdiff_t])


quiclibs=
tlslibs=
rapidolibs=

AC_ARG_WITH([picotls],
            [AS_HELP_STRING([--with-picotls], [location of picotls, if not installed globally])],
            [],
            [with_picotls=../picotls/])

LDFLAGS="-L$with_picotls $LDFLAGS"

save_LIBS="$LIBS"
AC_CHECK_LIB([picotls-core],[ptls_calc_hash], [have_tls=yes])
if test "$have_tls" = "yes"; then
    tlslibs="-lpicotls-core -lpicotls-openssl"
fi

LIBS="$tlslibs $save_LIBS"

AC_CHECK_LIB([picotls-fusion],[ptls_fusion_aesgcm_encrypt],[have_fusion=yes])

if test "$have_fusion" = "yes"; then
    tlslibs="$tlslibs -lpicotls-fusion"
fi

LIBS="$tlslibs $save_LIBS"

AC_ARG_WITH([picoquic],
            [AS_HELP_STRING([--with-picoquic], [location of picoquic, if not installed globally])],
            [],
            [with_picoquic=../picoquic/])

LDFLAGS="-L$with_picoquic $LDFLAGS"
CFLAGS="-I$with_picoquic/picoquic/ $CFLAGS"

AC_CHECK_LIB([picoquic-core],[picoquic_create],[have_picoquic=yes])
if test "$have_picoquic" = "yes"; then
    quiclibs="-lpicoquic-core -lpicoquic-log"

    AC_DEFINE([HAVE_QUIC],[1],[Have QUIC support])
    AC_SUBST([HAVE_QUIC],[1])
else
    echo "WARNING! Picoquic was not found! Did you 'make install' it?"
fi

AC_ARG_WITH([rapido],
            [AS_HELP_STRING([--with-rapido], [location of rapido, if not installed globally])],
            [],
            [with_rapido=../rapido/])

LDFLAGS="-L$with_rapido $LDFLAGS"

AC_CHECK_LIB([picotls-rapido],[rapido_new_server],[have_rapido=yes])
if test "$have_rapido" = "yes"; then
    rapidolibs="-lpicotls-rapido"

    AC_DEFINE([HAVE_RAPIDO],[1],[Have RAPIDO support])
    AC_SUBST([HAVE_RAPIDO],[1])
else
    echo "INFO: Rapido was not found, support is not activated."
fi
LIBS="$save_LIBS"


AC_SUBST(RAPIDOLIBS, $rapidolibs)
AC_SUBST(TLSLIBS, $tlslibs)
AC_SUBST(QUICLIBS, $quiclibs)
AC_SUBST(PREFIX, $prefix)

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE

AC_CONFIG_FILES([stamp-h], [echo timestamp > stamp-h])
AC_CONFIG_FILES(Makefile)
AC_OUTPUT


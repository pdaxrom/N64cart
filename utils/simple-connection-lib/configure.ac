AC_INIT(libsimple_connection, 1.0)
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
AC_PROG_CC
AC_PROG_CXX
AC_PROG_LIBTOOL

AC_PATH_PROG([PKG_CONFIG], [pkg-config])

case $host_os in
linux*)
    CFLAGS="$CFLAGS -D_GNU_SOURCE"
    ;;
esac

AC_ARG_ENABLE(ssl,
[  --enable-ssl            enable ssl],
ENABLE_SSL=$enableval,
ENABLE_SSL=yes)
if test "${ENABLE_SSL}" = "yes"; then
    C_DEFINES="-DENABLE_SSL"
    CFLAGS="$CFLAGS $C_DEFINES"

    PKG_CHECK_MODULES(OPENSSL, libssl libcrypto)
    PKG_CHECK_MODULES_STATIC(OPENSSL_STATIC, libssl libcrypto)
fi

CFLAGS="$CFLAGS -Wall -Wpedantic"

AC_SUBST(C_DEFINES)
AC_SUBST(LIBTOOL_DEPS)
AC_SUBST([OPENSSL_INCLUDES])
AC_SUBST([OPENSSL_LIBS])
AC_SUBST([OPENSSL_STATIC_INCLUDES])
AC_SUBST([OPENSSL_STATIC_LIBS])
AC_LTDL_DLLIB
AC_CONFIG_FILES([src/Makefile
                 test/Makefile
                 Makefile
                 simple-connection.pc])
AC_OUTPUT

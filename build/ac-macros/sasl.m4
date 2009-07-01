dnl
dnl  SVN_LIB_SASL
dnl
dnl  Check configure options and assign variables related to
dnl  the sasl library.
dnl
dnl  If we find the library, set the shell variable
dnl  `svn_lib_sasl' to `yes'.  Otherwise, set `svn_lib_sasl'
dnl  to `no'.

AC_DEFUN(SVN_LIB_SASL,
[
  AC_ARG_WITH(sasl, [AS_HELP_STRING([--with-sasl=PATH],
                                    [Compile with libsasl2 in PATH])],
  [
    with_sasl="$withval"
    required="yes"
  ],
  [
    with_sasl="yes"
    required="no"
  ])

  AC_MSG_CHECKING([whether to look for SASL])

  if test "${with_sasl}" = "no"; then
    AC_MSG_RESULT([no])
    svn_lib_sasl=no
  else
    AC_MSG_RESULT([yes])
    saved_LDFLAGS="$LDFLAGS"
    saved_CPPFLAGS="$CPPFLAGS"

    if test "$with_sasl" = "yes"; then
      AC_MSG_NOTICE([Looking in default locations])
      AC_CHECK_HEADER(sasl/sasl.h,
        [AC_CHECK_HEADER(sasl/saslutil.h,
         [AC_CHECK_LIB(sasl2, prop_get, 
                       svn_lib_sasl=yes,
                       svn_lib_sasl=no)],
                       svn_lib_sasl=no)], svn_lib_sasl=no)
      if test "$svn_lib_sasl" = "no"; then
        with_sasl="/usr/local"
      fi
    else
      svn_lib_sasl=no
    fi

    if test "$svn_lib_sasl" = "no"; then
      SVN_SASL_INCLUDES="-I${with_sasl}/include"
      CPPFLAGS="$CPPFLAGS $SVN_SASL_INCLUDES"
      LDFLAGS="$LDFLAGS -L${with_sasl}/lib"

      AC_CHECK_HEADER(sasl/sasl.h,
        [AC_CHECK_HEADER(sasl/saslutil.h,
         [AC_CHECK_LIB(sasl2, prop_get, 
                       svn_lib_sasl=yes,
                       svn_lib_sasl=no)],
                       svn_lib_sasl=no)], svn_lib_sasl=no)
    fi

    AC_MSG_CHECKING([for availability of Cyrus SASL v2])
    if test "$svn_lib_sasl" = "yes"; then
      SVN_SASL_LIBS="-lsasl2"
      AC_MSG_RESULT([yes])
    else
      AC_MSG_RESULT([no])

      if test "$required" = "yes"; then
        dnl The user explicitly requested SASL, but we couldn't find it.
        dnl Exit with an error message.
        SVN_MSG_ERROR([Could not find Cyrus SASL v2])
      fi
      
      SVN_SASL_INCLUDES=""
      LDFLAGS="$saved_LDFLAGS"
    fi

    CPPFLAGS="$saved_CPPFLAGS"
  fi
    
  AC_SUBST(SVN_SASL_INCLUDES)
  AC_SUBST(SVN_SASL_LIBS)
])

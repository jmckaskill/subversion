dnl check to see if SWIG is current enough.
dnl
dnl if it is, then check to see if we have the correct version of python.
dnl
dnl if we do, then set up the appropriate SWIG_ variables to build the 
dnl python bindings.

AC_DEFUN(SVN_CHECK_SWIG,
[
  AC_ARG_WITH(swig,
              AC_HELP_STRING([--with-swig=PATH],
                             [Try to use 'PATH/bin/swig' to build the
                              swig bindings.  If PATH is not specified,
                              look for a 'swig' binary in your PATH.]),
  [
    case "$withval" in
      "no")
        SWIG_SUITABLE=no
      ;;
      "yes")
        SVN_FIND_SWIG(check)
      ;;
      *)
        SVN_FIND_SWIG($withval)
      ;;
    esac
  ],
  [
    SVN_FIND_SWIG(check)
  ])
])

AC_DEFUN(SVN_FIND_SWIG,
[
  where=$1

  if test $where = check; then
    AC_PATH_PROG(SWIG, swig, none)
  else
    SWIG=$where/bin/swig
  fi

  if test "$SWIG" != "none"; then
    AC_MSG_CHECKING([swig version])
    SWIG_VERSION="`$SWIG -version 2>&1 | sed -ne 's/^.*Version \(.*\)$/\1/p'`"
    AC_MSG_RESULT([$SWIG_VERSION])
    # If you change the required swig version number, don't forget to update:
    #   subversion/bindings/swig/INSTALL
    #   subversion/bindings/swig/README
    #   packages/rpm/mandrake-9.0/subversion.spec
    #   packages/rpm/redhat-7.x/subversion.spec
    #   packages/rpm/redhat-8.x/subversion.spec
    case $SWIG_VERSION in
        [1.3.1[679]*])
          SWIG_SUITABLE=yes
          ;;
        *)
          SWIG_SUITABLE=no
          AC_MSG_WARN([swig bindings require 1.3.19.])
          ;;
    esac
    if test "$PYTHON" != "none" -a "$SWIG_SUITABLE" = "yes"; then
      ### TODO: When it's ready, add the swig-java-lib rule here.
      SWIG_BUILD_RULES="$SWIG_BUILD_RULES swig-py-lib"
      SWIG_INSTALL_RULES="$SWIG_INSTALL_RULES install-swig-py-lib"

      AC_CACHE_CHECK([for swig library directory], [ac_cv_swig_swiglib_dir],[
        ac_cv_swig_swiglib_dir="`$SWIG -swiglib`"
      ])
      SWIG_LIBSWIG_DIR="$ac_cv_swig_swiglib_dir"

      AC_CACHE_CHECK([if swig needs -L for its libraries],
        [ac_cv_swig_ldflags],[
        # The swig libraries are one directory above the
        # `swig -swiglib` directory.
        ac_cv_swig_ldflags=""
        swig_lib_dir="`dirname $ac_cv_swig_swiglib_dir`"
        if test "$swig_lib_dir" &&
           test "$swig_lib_dir" != "/lib" &&
           test "$swig_lib_dir" != "/usr/lib"; then
          ac_cv_swig_ldflags="-L$swig_lib_dir"
        fi
      ])
      SWIG_LDFLAGS="$ac_cv_swig_ldflags"

      AC_CACHE_CHECK([for Python includes], [ac_cv_python_includes],[
        ac_cv_python_includes="`$PYTHON ${abs_srcdir}/build/get-py-info.py --includes`"
      ])
      SWIG_PY_INCLUDES="$ac_cv_python_includes"

      AC_CACHE_CHECK([for compiling Python extensions], [ac_cv_python_compile],[
        ac_cv_python_compile="`$PYTHON ${abs_srcdir}/build/get-py-info.py --compile`"
      ])
      SWIG_PY_COMPILE="$ac_cv_python_compile"

      AC_CACHE_CHECK([for linking Python extensions], [ac_cv_python_link],[
        ac_cv_python_link="`$PYTHON ${abs_srcdir}/build/get-py-info.py --link`"
      ])
      SWIG_PY_LINK="$ac_cv_python_link"
    fi

    if test "$JDK" != "none" -a "$SWIG_SUITABLE" = "yes"; then
      list="`find "$JDK/include" -type d -print`"
      for dir in $list; do
        SWIG_JAVA_INCLUDES="$SWIG_JAVA_INCLUDES -I$dir"
      done
    fi

    if test "$PERL" != "none" -a "$SWIG_SUITABLE" = "yes"; then
      SWIG_PL_INCLUDES="`$PERL -MExtUtils::Embed -e ccopts`"
      SWIG_PL_COMPILE="`$PERL -MConfig -e 'print $Config{cc}'` \$(SWIG_PL_INCLUDES)"
      SWIG_PL_LINK="`$PERL -MConfig -e 'print $Config{ld}'` `$PERL -MConfig -e 'print $Config{lddlflags}'` `$PERL -MExtUtils::Embed -e ldopts`"
    fi

  fi
  AC_SUBST(SWIG_BUILD_RULES)
  AC_SUBST(SWIG_INSTALL_RULES)
  AC_SUBST(SWIG_PY_INCLUDES)
  AC_SUBST(SWIG_PY_COMPILE)
  AC_SUBST(SWIG_PY_LINK)
  AC_SUBST(SWIG_JAVA_INCLUDES)
  AC_SUBST(SWIG_PL_INCLUDES)
  AC_SUBST(SWIG_PL_COMPILE)
  AC_SUBST(SWIG_PL_LINK)
  AC_SUBST(SWIG_LIBSWIG_DIR)
  AC_SUBST(SWIG_LDFLAGS)
])

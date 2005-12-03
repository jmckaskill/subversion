dnl -----------------------------------------------------------------------
dnl Additional autoconf macros for Subversion
dnl


dnl this writes a "config.nice" file which reinvokes ./configure with all
dnl of the arguments. this is different from config.status which simply
dnl regenerates the output files. config.nice is useful after you rebuild
dnl ./configure (via autoconf or autogen.sh)
AC_DEFUN(SVN_CONFIG_NICE,[
  AC_REQUIRE([AC_CANONICAL_HOST])
  AC_MSG_NOTICE([creating $1])
  rm -f $1
  cat >$1<<EOF
#! /bin/sh
#
# Created by configure

EOF

  case $host in
    *-*-cygwin*)
      # exec closes config.nice before configure attempts to rewrite it
      EXEC_HACK="exec "
      ;;
    *)
      EXEC_HACK=
      ;;
  esac

  echo "$EXEC_HACK\"[$]0\" \\" >> $1
  for arg in "[$]@"; do
    case $arg in
      --no-create) ;;
      --no-recursion) ;;
      *)
        echo "\"$arg\" \\" >> $1
      ;;
    esac
  done
  echo '"[$]@"' >> $1
  chmod +x $1
])

dnl
dnl SVN_SUBDIR_CONFIG(dir [, sub-package-cmdline-args])
dnl
dnl Note that this code is a direct copy of that which is found in 
dnl the apr project's build/apr_common.m4.
AC_DEFUN(SVN_SUBDIR_CONFIG, [
  if test "$do_subdir_config" = "yes" ; then
    # save our work to this point; this allows the sub-package to use it
    AC_CACHE_SAVE

    echo "configuring package in $1 now"
    ac_popdir=`pwd`
    ac_abs_srcdir=`(cd $srcdir/$1 && pwd)`
    apr_config_subdirs="$1"
    test -d $1 || $MKDIR $1
    cd $1

    # A "../" for each directory in /$config_subdirs.
    ac_dots=[`echo $apr_config_subdirs|sed -e 's%^\./%%' -e 's%[^/]$%&/%' -e 's%[^/]*/%../%g'`]

    # Make the cache file name correct relative to the subdirectory.
    case "$cache_file" in
    /*) ac_sub_cache_file=$cache_file ;;
    *) # Relative path.
      ac_sub_cache_file="$ac_dots$cache_file" ;;
    esac

    # The eval makes quoting arguments work.
    if eval $SHELL $ac_abs_srcdir/configure $ac_configure_args --cache-file=$ac_sub_cache_file --srcdir=$ac_abs_srcdir $2
    then :
      echo "$1 configured properly"
    else
      echo "configure failed for $1"
      exit 1
    fi
    cd $ac_popdir

    # grab any updates from the sub-package
    AC_CACHE_LOAD
  else
    AC_MSG_WARN(not running configure in $1)
  fi
])dnl

dnl
dnl SVN_CONFIG_SCRIPT(path)
dnl
dnl Make AC_OUTPUT create an executable file.
dnl Accumulate filenames in $SVN_CONFIG_SCRIPT_FILES for AC_SUBSTing to
dnl use in, for example, Makefile distclean rules.
dnl
AC_DEFUN(SVN_CONFIG_SCRIPT, [
  SVN_CONFIG_SCRIPT_FILES="$SVN_CONFIG_SCRIPT_FILES $1"
  AC_CONFIG_FILES([$1], [chmod +x $1])])

dnl Iteratively interpolate the contents of the second argument
dnl until interpolation offers no new result. Then assign the
dnl final result to $1.
dnl
dnl Based on APR_EXPAND_VAR macro
dnl
dnl Example:
dnl
dnl foo=1
dnl bar='${foo}/2'
dnl baz='${bar}/3'
dnl SVN_EXPAND_VAR(fraz, $baz)
dnl   $fraz is now "1/2/3"
dnl 
AC_DEFUN(SVN_EXPAND_VAR,[
svn_last=
svn_cur="$2"
while test "x${svn_cur}" != "x${svn_last}";
do
  svn_last="${svn_cur}"
  svn_cur=`eval "echo ${svn_cur}"`
done
$1="${svn_cur}"
])

dnl SVN_MAYBE_ADD_TO_CFLAGS(option)
dnl
dnl Attempt to compile a trivial C program to test if the option passed
dnl is valid. If it is, then add it to CFLAGS. with the passed in option
dnl and see if it was successfully compiled.
dnl
dnl This macro is usually used for stricter syntax checking flags.
dnl Therefore we include certain headers which may in turn include system
dnl headers, as system headers on some platforms may fail strictness checks
dnl we wish to use on other platforms.

AC_DEFUN(SVN_MAYBE_ADD_TO_CFLAGS,
[
  option="$1"
  svn_maybe_add_to_cflags_saved_flags="$CFLAGS"
  CFLAGS="$CFLAGS $option"
  AC_MSG_CHECKING([if $CC accepts $option])
  AC_TRY_COMPILE(
    [#include <apr_portable.h>],
    [],
    [svn_maybe_add_to_cflags_ok="yes"],
    [svn_maybe_add_to_cflags_ok="no"]
  )
  if test "$svn_maybe_add_to_cflags_ok" = "yes"; then
    AC_MSG_RESULT([yes, will use it])
  else
    AC_MSG_RESULT([no])
    CFLAGS="$svn_maybe_add_to_cflags_saved_flags"
  fi
])

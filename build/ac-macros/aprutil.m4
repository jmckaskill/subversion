dnl  SVN_LIB_APRUTIL(wanted_regex, alt_wanted_regex)
dnl
dnl  'wanted_regex' and 'alt_wanted_regex are regular expressions 
dnl  that the aprutil version string must match.
dnl
dnl  Check configure options and assign variables related to
dnl  the Apache Portable Runtime Utilities (APRUTIL) library.
dnl
dnl  If there is an apr-util source directory, there *must* be a
dnl  corresponding apr source directory.  APRUTIL's build system
dnl  is too tied in with apr.  (You can't use an installed APR and
dnl  a source APR-util.)
dnl


AC_DEFUN(SVN_LIB_APRUTIL,
[
  APRUTIL_WANTED_REGEX="$1"
  APRUTIL_WANTED_REGEX_TOO="$2"

  AC_MSG_NOTICE([Apache Portable Runtime Utility (APRUTIL) library configuration])

  APR_FIND_APU("$srcdir/apr-util", "./apr-util", 1, [0 1])

  if test $apu_found = "no"; then
    AC_MSG_WARN([APRUTIL not found])
    SVN_DOWNLOAD_APRUTIL
  fi

  if test $apu_found = "reconfig"; then
    SVN_SUBDIR_CONFIG(apr-util, --with-apr=../apr)
    SVN_SUBDIRS="$SVN_SUBDIRS apr-util"
  fi

  dnl check APRUTIL version number against regex  

  AC_MSG_CHECKING([APR-UTIL version])    
  apu_version="`$apu_config --version`"
  if test $? -ne 0; then
    # This is a hack as suggested by Ben Collins-Sussman.  It can be
    # removed after apache 2.0.44 has been released.  (The apu-config
    # shipped in 2.0.43 contains a correct version number, but
    # stupidly doesn't understand the --version switch.)
    apu_version=`grep "APRUTIL_DOTTED_VERSION=" $(which $apu_config) | tr -d "APRUTIL_DOTTED_VERSION="| tr -d '"'`
    #AC_MSG_ERROR([
    #    apu-config --version failed.
    #    Your apu-config doesn't support the --version switch, please upgrade
    #    to APR-UTIL more recent than 2002-Nov-05.])
  fi
  AC_MSG_RESULT([$apu_version])

  if test `expr $apu_version : $APRUTIL_WANTED_REGEX` -eq 0 \
       -a `expr $apu_version : $APRUTIL_WANTED_REGEX_TOO` -eq 0; then
    echo "wanted regex is $APRUTIL_WANTED_REGEX or $APRUTIL_WANTED_REGEX_TOO"
    AC_MSG_ERROR([invalid apr-util version found])
  fi

  dnl Get libraries and thread flags from APRUTIL ---------------------

  LDFLAGS="$LDFLAGS `$apu_config --ldflags`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apu-config --ldflags failed])
  fi

  SVN_APRUTIL_INCLUDES="`$apu_config --includes`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apu-config --includes failed])
  fi

  dnl When APR stores the dependent libs in the .la file, we don't need
  dnl --libs.
  SVN_APRUTIL_LIBS="`$apu_config --link-libtool --libs`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apu-config --link-libtool --libs failed])
  fi

  SVN_APRUTIL_EXPORT_LIBS="`$apu_config --link-ld --libs`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apu-config --link-ld --libs failed])
  fi

  AC_SUBST(SVN_APRUTIL_INCLUDES)
  AC_SUBST(SVN_APRUTIL_LIBS)
  AC_SUBST(SVN_APRUTIL_EXPORT_LIBS)
  AC_SUBST(SVN_APRUTIL_PREFIX)
])

dnl SVN_DOWNLOAD_APRUTIL()
dnl no apr-util found, print out a message telling the user what to do
AC_DEFUN(SVN_DOWNLOAD_APRUTIL,
[
  echo "The Apache Portable Runtime Utility (APRUTIL) library cannot be found."
  echo "Either install APRUTIL on this system and supply the appropriate"
  echo "--with-apr-util option"
  echo ""
  echo "or"
  echo ""
  echo "get it with SVN and put it in a subdirectory of this source:"
  echo ""
  echo "   svn co \\"
  echo "    http://svn.apache.org/repos/asf/apr/apr-util/branches/APU_0_9_BRANCH \\"
  echo "    apr-util"
  echo ""
  echo "Run that right here in the top level of the Subversion tree,"
  echo "then run autogen.sh again."
  echo ""
  AC_MSG_ERROR([no suitable APRUTIL found])
])

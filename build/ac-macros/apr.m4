dnl ===================================================================
dnl   Licensed to the Apache Software Foundation (ASF) under one
dnl   or more contributor license agreements.  See the NOTICE file
dnl   distributed with this work for additional information
dnl   regarding copyright ownership.  The ASF licenses this file
dnl   to you under the Apache License, Version 2.0 (the
dnl   "License"); you may not use this file except in compliance
dnl   with the License.  You may obtain a copy of the License at
dnl
dnl     http://www.apache.org/licenses/LICENSE-2.0
dnl
dnl   Unless required by applicable law or agreed to in writing,
dnl   software distributed under the License is distributed on an
dnl   "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
dnl   KIND, either express or implied.  See the License for the
dnl   specific language governing permissions and limitations
dnl   under the License.
dnl ===================================================================
dnl
dnl  SVN_LIB_APR(wanted_regex, alt_wanted_regex)
dnl
dnl  'wanted_regex' and 'alt_wanted_regex are regular expressions
dnl  that the apr version string must match.
dnl
dnl  Check configure options and assign variables related to
dnl  the Apache Portable Runtime (APR) library.
dnl

AC_DEFUN(SVN_LIB_APR,
[
  APR_WANTED_REGEXES="$1"

  AC_MSG_NOTICE([Apache Portable Runtime (APR) library configuration])

  APR_FIND_APR("$abs_srcdir/apr", "$abs_builddir/apr", 1, [1 0])

  if test $apr_found = "no"; then
    AC_MSG_WARN([APR not found])
    SVN_DOWNLOAD_APR
  fi

  if test $apr_found = "reconfig"; then
    SVN_EXTERNAL_PROJECT([apr])
  fi

  dnl check APR version number against regex  

  AC_MSG_CHECKING([APR version])    
  apr_version="`$apr_config --version`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --version failed])
  fi
  AC_MSG_RESULT([$apr_version])

  APR_WANTED_REGEX_MATCH=0
  for apr_wanted_regex in $APR_WANTED_REGEXES; do
    if test `expr $apr_version : $apr_wanted_regex` -ne 0; then
      APR_WANTED_REGEX_MATCH=1
      break
    fi
  done
      
  if test $APR_WANTED_REGEX_MATCH -eq 0; then
    echo "wanted regexes are $APR_WANTED_REGEXES"
    AC_MSG_ERROR([invalid apr version found])
  fi

  dnl Get build information from APR

  CPPFLAGS="$CPPFLAGS `$apr_config --cppflags`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --cppflags failed])
  fi

  CFLAGS="$CFLAGS `$apr_config --cflags`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --cflags failed])
  fi

  apr_ldflags="`$apr_config --ldflags`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --ldflags failed])
  fi
  LDFLAGS="$LDFLAGS `SVN_REMOVE_STANDARD_LIB_DIRS($apr_ldflags)`"

  SVN_APR_INCLUDES="`$apr_config --includes`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --includes failed])
  fi

  SVN_APR_PREFIX="`$apr_config --prefix`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --prefix failed])
  fi

  if test "$enable_all_static" = "yes"; then
    SVN_APR_LIBS="`$apr_config --link-ld --libs`"
    if test $? -ne 0; then
      AC_MSG_ERROR([apr-config --link-ld --libs failed])
    fi
  else
    SVN_APR_LIBS="`$apr_config --link-ld`"
    if test $? -ne 0; then
      AC_MSG_ERROR([apr-config --link-ld failed])
    fi
  fi
  SVN_APR_LIBS="`SVN_REMOVE_STANDARD_LIB_DIRS($SVN_APR_LIBS)`"

  SVN_APR_SHLIB_PATH_VAR="`$apr_config --shlib-path-var`"
  if test $? -ne 0; then
    AC_MSG_ERROR([apr-config --shlib-path-var failed])
  fi

  AC_SUBST(SVN_APR_PREFIX)
  AC_SUBST(SVN_APR_CONFIG, ["$apr_config"])
  AC_SUBST(SVN_APR_INCLUDES)
  AC_SUBST(SVN_APR_LIBS)
  AC_SUBST(SVN_APR_SHLIB_PATH_VAR)
])

dnl SVN_DOWNLOAD_APR()
dnl no apr found, print out a message telling the user what to do
AC_DEFUN(SVN_DOWNLOAD_APR,
[
  echo "The Apache Portable Runtime (APR) library cannot be found."
  echo "Please install APR on this system and supply the appropriate"
  echo "--with-apr option to 'configure'"
  echo ""
  echo "or"
  echo ""
  echo "get it with SVN and put it in a subdirectory of this source:"
  echo ""
  echo "   svn co \\"
  echo "    http://svn.apache.org/repos/asf/apr/apr/branches/1.3.x \\"
  echo "    apr"
  echo ""
  echo "Run that right here in the top level of the Subversion tree."
  echo "Afterwards, run apr/buildconf in that subdirectory and"
  echo "then run configure again here."
  echo ""
  echo "Whichever of the above you do, you probably need to do"
  echo "something similar for apr-util, either providing both"
  echo "--with-apr and --with-apr-util to 'configure', or"
  echo "getting both from SVN with:"
  echo ""
  echo "   svn co \\"
  echo "    http://svn.apache.org/repos/asf/apr/apr-util/branches/1.3.x \\"
  echo "    apr-util"
  echo ""
  AC_MSG_ERROR([no suitable apr found])
])

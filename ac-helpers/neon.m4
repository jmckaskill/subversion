dnl   SVN_LIB_NEON(version)
dnl
dnl   Search for a suitable version of neon, it must be 
dnl   the exact version 'version'.
dnl
dnl   If there is a neon/ subdir we assume we want to use it.
dnl   If the subdir is the wrong version we exit with a failure
dnl   regardless if neon is installed somewhere else on the system.
dnl
dnl   If there isn't a neon/ subdir then we look for 'neon-config'
dnl   in PATH (or the location specified by a --with-neon=PATH 
dnl   switch).  

AC_DEFUN(SVN_LIB_NEON,
[

  NEON_WANTED="$1"
  NEON_URL="http://www.webdav.org/neon/neon-${NEON_WANTED}.tar.gz"

  AC_MSG_NOTICE([checking neon library])

  AC_ARG_WITH(neon,
              [AC_HELP_STRING([--with-neon=PREFIX], 
	      [Determine neon library configuration based on 
	      'PREFIX/bin/neon-config'. Default is to search for neon 
	      in a subdirectory of the top source directory and then to
	      look for neon-config in $PATH.])],
  [
    if test -d $abs_srcdir/neon ; then
      AC_MSG_ERROR([--with-neon option but neon/ subdir exists.
Please either remove that subdir or don't use the --with-neon option.])
    else
      if test "$withval" = "yes" ; then
        AC_PATH_PROG(neon_config,neon-config)
      else
        neon_config="$withval/bin/neon-config"
      fi

      SVN_NEON_CONFIG()
    fi
  ],
  [
    if test -d $abs_srcdir/neon ; then
      AC_MSG_CHECKING([neon library version])
      NEON_VERSION=`$abs_srcdir/ac-helpers/get-neon-ver.sh $abs_srcdir/neon`
      AC_MSG_RESULT([$NEON_VERSION])
      if test "$NEON_WANTED" != "$NEON_VERSION"; then
        echo "You have a neon/ subdir containing version $NEON_VERSION,"
        echo "but Subversion needs neon ${NEON_WANTED}."
        SVN_DOWNLOAD_NEON()
      else
        echo "Using neon found in source directory."
        SVN_NEON_INCLUDES=-'I$(abs_srcdir)/neon/src'
        NEON_LIBS="\$(abs_builddir)/neon/src/libneon.la"

dnl Configure neon --------------------------
        # The arguments passed to this configure script are passed down to
        # neon's configure script, but, since neon defaults to *not* building
        # shared libs, and we default to building shared libs, we have to 
        # explicitly pass down an --{enable,disable}-shared argument, to make
        # sure neon does the same as we do.
        if test "$enable_shared" = "yes"; then
          args="--enable-shared"
        else
          args="--disable-shared"
        fi
  
        SVN_SUBDIR_CONFIG(neon, $args --with-expat="$abs_srcdir/expat-lite/libexpat.la")

        if test -f "$abs_builddir/neon/neon-config" ; then
          AC_MSG_CHECKING([for any extra libraries neon needs])
          # this is not perfect since it will pick up extra -L flags too,
          # but that shouldn't do any real damage.
          NEON_LIBS_NEW=`$SHELL $abs_builddir/neon/neon-config --libs | sed -e"s/-lneon//g"`
          AC_MSG_RESULT([$NEON_LIBS_NEW])
          NEON_LIBS="$NEON_LIBS $NEON_LIBS_NEW"
          # Also find out which macros neon defines (but ignore extra include paths):
          # this will include -DNEON_SSL if neon was built with SSL support
          changequote(<<, >>)dnl
          CFLAGS="$CFLAGS `$SHELL $abs_builddir/neon/neon-config --cflags | sed -e "s/-I[^ ]*//g"`"
          changequote([, ])dnl
        fi

	SVN_SUBDIRS="$SVN_SUBDIRS neon"
      fi
    else
      # no --with-neon switch, and no neon subdir, look in PATH
      AC_PATH_PROG(neon_config,neon-config)
      SVN_NEON_CONFIG()
    fi

  ])
  
  # neon should not add -I/usr/include to --cflags; remove this when it doesn't.
  # Leaving it in means a system DB3 etc could be picked up.
  SVN_NEON_INCLUDES=`echo $SVN_NEON_INCLUDES | sed -e "s|-I/usr/include ||g" \
                     -e "s|-I/usr/include$||g"`

  AC_SUBST(SVN_NEON_INCLUDES)
  AC_SUBST(NEON_LIBS)
])

dnl SVN_NEON_CONFIG()
dnl neon-config found, gather relevant information from it
AC_DEFUN(SVN_NEON_CONFIG,
[
  if test "$neon_config" != ""; then
    AC_MSG_CHECKING([neon library version])
    NEON_VERSION=`$neon_config --version | sed -e 's/^neon //'`
    AC_MSG_RESULT([$NEON_VERSION])

    if test "$NEON_WANTED" != "$NEON_VERSION"; then
      echo "You have neon version $NEON_VERSION,"
      echo "but Subversion needs neon $NEON_WANTED."
      SVN_DOWNLOAD_NEON()
    else
      changequote(<<, >>)dnl
      SVN_NEON_INCLUDES=`$neon_config --cflags | sed -e 's/-D[^ ]*//g'`
      NEON_LIBS=`$neon_config --libs | sed -e 's/-lneon//g'`
      CFLAGS="$CFLAGS `$neon_config --cflags | sed -e 's/-I[^ ]*//g'`"
      changequote([, ])dnl
      NEON_LIBS="$NEON_LIBS "`$neon_config --prefix `"/lib/libneon.la"
    fi
  else
    # no neon subdir, no neon-config in PATH
    AC_MSG_RESULT([nothing])
    echo "No suitable neon can be found."
    SVN_DOWNLOAD_NEON()
  fi
])

dnl SVN_DOWNLOAD_NEON()
dnl no neon found, print out a message telling the user what to do
AC_DEFUN(SVN_DOWNLOAD_NEON,
[
  echo "Please either install neon ${NEON_WANTED} on this system"
  echo ""
  echo "or"
  echo ""
  echo "get neon ${NEON_WANTED} from:"
  echo "    ${NEON_URL}"
  echo "unpack the archive using tar/gunzip and rename the resulting"
  echo "directory from ./neon-${NEON_WANTED}/ to ./neon/"
  AC_MSG_ERROR([no suitable neon found])
])

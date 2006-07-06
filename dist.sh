#!/bin/sh

#
# USAGE: ./dist.sh -v VERSION -r REVISION [-rs REVISION-SVN] [-pr REPOS-PATH]
#                  [-apr PATH-TO-APR ] [-apru PATH-TO-APR-UTIL] 
#                  [-apri PATH-TO-APR-ICONV] [-neon PATH-TO-NEON]
#                  [-alpha ALPHA_NUM|-beta BETA_NUM|-rc RC_NUM]
#                  [-zip] [-sign] [-nodeps]
#
#   Create a distribution tarball, labelling it with the given VERSION.
#   The REVISION or REVISION-SVN will be used in the version string.
#   The tarball will be constructed from the root located at REPOS-PATH.
#   If REPOS-PATH is not specified then the default is "branches/VERSION".
#   For example, the command line:
#
#      ./dist.sh -v 0.24.2 -r 6284
#
#   from the top-level of a branches/0.24.2 working copy will create
#   the 0.24.2 release tarball. Make sure you have apr, apr-util,
#   and neon subdirectories in your current working directory or
#   specify the path to them with the -apr, -apru or -neon options.
#   For example:
#      ./dist.sh -v 1.1.0 -r 10277 -pr branches/1.1.x \
#        -apr -neon ~/in-tree-libraries/neon-0.24.7 \
#        -apr ~/in-tree-libraries/httpd-2.0.50/srclib/apr \
#        -apru ~/in-tree-libraries/httpd-2.0.50/srclib/apr-util/
#
#   When building alpha, beta or rc tarballs pass the appropriate flag
#   followed by the number for that release.  For example you'd do
#   the following for a Beta 1 release:
#      ./dist.sh -v 1.1.0 -r 10277 -pr branches/1.1.x -beta 1
# 
#   If neither an -alpha, -beta or -rc option with a number is
#   specified, it will build a release tarball.
#  
#   To build a Windows zip file package pass -zip and the path
#   to apr-iconv with -apri.


# A quick and dirty usage message
USAGE="USAGE: ./dist.sh -v VERSION -r REVISION \
[-rs REVISION-SVN ] [-pr REPOS-PATH] \
[-alpha ALPHA_NUM|-beta BETA_NUM|-rc RC_NUM] \
[-apr APR_PATH ] [-apru APR_UTIL_PATH] [-apri APR_ICONV_PATH] \
[-neon NEON_PATH ] [-zip] [-sign] [-nodeps]
 EXAMPLES: ./dist.sh -v 0.36.0 -r 8278
           ./dist.sh -v 0.36.0 -r 8278 -pr trunk
           ./dist.sh -v 0.36.0 -r 8282 -rs 8278 -pr tags/0.36.0
           ./dist.sh -v 0.36.0 -r 8282 -rs 8278 -pr tags/0.36.0 -alpha
           ./dist.sh -v 0.36.0 -r 8282 -rs 8278 -pr tags/0.36.0 -beta 1"

# Let's check and set all the arguments
ARG_PREV=""

for ARG in $@
do
  if [ "$ARG_PREV" ]; then

    case $ARG_PREV in
         -v)  VERSION="$ARG" ;;
         -r)  REVISION="$ARG" ;;
        -rs)  REVISION_SVN="$ARG" ;;
        -pr)  REPOS_PATH="$ARG" ;;
	-rc)  RC="$ARG" ;;
       -apr)  APR_PATH="$ARG" ;;
       -apru) APRU_PATH="$ARG" ;;
       -apri) APRI_PATH="$ARG" ;;
      -neon)  NEON_PATH="$ARG" ;;
      -beta)  BETA="$ARG" ;;
     -alpha)  ALPHA="$ARG" ;;
          *)  ARG_PREV=$ARG ;;
    esac

    ARG_PREV=""

  else

    case $ARG in
      -v|-r|-rs|-pr|-beta|-rc|-alpha|-apr|-apru|-apri|-neon)
        ARG_PREV=$ARG
        ;;
      -zip)
        ZIP=1
        ARG_PREV=""
        ;;
      -nodeps)
        NODEPS=1
        ARG_PREV=""
        ;;
      -sign)
        SIGN=1
        ARG_PREV=""
        ;;
      *)
        echo " $USAGE"
        exit 1
        ;;
    esac
  fi
done

if [ -z "$REVISION_SVN" ]; then
  REVISION_SVN=$REVISION
fi

if [ -n "$ALPHA" ] && [ -n "$BETA" ] ||
   [ -n "$ALPHA" ] && [ -n "$RC" ] ||
   [ -n "$BETA" ] && [ -n "$RC" ] ; then
  echo " $USAGE"
  exit 1
elif [ -n "$ALPHA" ] ; then
  VER_TAG="Alpha $ALPHA"
  VER_NUMTAG="-alpha$ALPHA" 
elif [ -n "$BETA" ] ; then
  VER_TAG="Beta $BETA"
  VER_NUMTAG="-beta$BETA"
elif [ -n "$RC" ] ; then
  VER_TAG="Release Candidate $RC"
  VER_NUMTAG="-rc$RC"
else
  VER_TAG="r$REVISION_SVN"
  VER_NUMTAG=""
fi
  
if [ -n "$ZIP" ] ; then
  EXTRA_EXPORT_OPTIONS="--native-eol CRLF"
fi

if [ -z "$VERSION" ] || [ -z "$REVISION" ] ; then
  echo " $USAGE"
  exit 1
fi

if [ -z "$APR_PATH" ]; then
  APR_PATH='apr'
fi

if [ -z "$APRU_PATH" ]; then
  APRU_PATH='apr-util'
fi

if [ -z "$NEON_PATH" ]; then
  NEON_PATH='neon'
fi

if [ -z "$APRI_PATH" ]; then
  APRI_PATH='apr-iconv'
fi

if [ -z "$REPOS_PATH" ]; then
  REPOS_PATH="branches/$VERSION"
else
  REPOS_PATH="`echo $REPOS_PATH | sed 's/^\/*//'`"
fi

# See comment when we 'roll' the tarballs as to why pax is required.
type pax > /dev/null 2>&1
if [ $? -ne 0 ] && [ -z "$ZIP" ]; then
  echo "ERROR: pax could not be found"
  exit 1
fi

# Default to 'wget', but allow 'curl' to be used if available.
HTTP_FETCH=wget
HTTP_FETCH_OUTPUT="-O"
type wget > /dev/null 2>&1
if [ $? -ne 0 ]; then
  type curl > /dev/null 2>&1
  if [ $? -ne 0 ]; then
    echo "Neither curl or wget found."
    exit 2
  fi
  HTTP_FETCH=curl
  HTTP_FETCH_OUTPUT="-o"
fi

DISTNAME="subversion-${VERSION}${VER_NUMTAG}"
DIST_SANDBOX=.dist_sandbox
DISTPATH="$DIST_SANDBOX/$DISTNAME"

echo "Distribution will be named: $DISTNAME"
echo " release branch's revision: $REVISION"
echo "     executable's revision: $REVISION_SVN"
echo "     constructed from path: /$REPOS_PATH"

rm -rf "$DIST_SANDBOX"
mkdir "$DIST_SANDBOX"
echo "Removed and recreated $DIST_SANDBOX"

LC_ALL=C
LANG=C
export LC_ALL
export LANG

echo "Exporting revision $REVISION of Subversion into sandbox..."
(cd "$DIST_SANDBOX" && \
 ${SVN:-svn} export -q $EXTRA_EXPORT_OPTIONS -r "$REVISION" \
     "http://svn.collab.net/repos/svn/$REPOS_PATH" \
     "$DISTNAME" --username none --password none)

install_dependency()
{
  DEP_NAME=$1
  if [ -z $2 ]; then
    DEP_PATH=/dev/null
  else
    DEP_PATH=$2
  fi

  if [ -d $DEP_PATH ]; then
    if [ -d $DEP_PATH/.svn ]; then
      echo "Exporting local $DEP_NAME into sandbox"
      ${SVN:-svn} export -q $EXTRA_EXPORT_OPTIONS "$DEP_PATH" "$DISTPATH/$DEP_NAME"
    else
      echo "Copying local $DEP_NAME into sandbox"
      cp -r "$DEP_PATH" "$DISTPATH/$DEP_NAME" 
      (cd "$DISTPATH/$DEP_NAME" && [ -f Makefile ] && make distclean)
      echo "Removing all CVS/ and .cvsignore files from $DEP_NAME..."
      find "$DISTPATH/$DEP_NAME" -name CVS -type d -print | xargs rm -fr
      find "$DISTPATH/$DEP_NAME" -name .cvsignore -print | xargs rm -f
      find "$DISTPATH/$DEP_NAME" -name '*.o' -print | xargs rm -f
    fi
  else
    # Not having the dependency directories isn't fatal if -nodeps passed.
    if [ -z "$NODEPS" ]; then
      echo "Missing dependency directory!"
      exit 2
    fi
  fi
}

install_dependency apr "$APR_PATH"
install_dependency apr-util "$APRU_PATH"

if [ -n "$ZIP" ]; then
  install_dependency apr-iconv "$APRI_PATH"
fi

install_dependency neon "$NEON_PATH"

find "$DISTPATH" -name config.nice -print | xargs rm -f

# Massage the new version number into svn_version.h.  We need to do
# this before running autogen.sh --release on the subversion code,
# because otherwise svn_version.h's mtime makes SWIG files regenerate
# on end-user's systems, when they should just be compiled by the
# Release Manager and left at that.

ver_major=`echo $VERSION | cut -d '.' -f 1`
ver_minor=`echo $VERSION | cut -d '.' -f 2`
ver_patch=`echo $VERSION | cut -d '.' -f 3`

vsn_file="$DISTPATH/subversion/include/svn_version.h"

sed \
 -e "/#define *SVN_VER_MAJOR/s/[0-9]\+/$ver_major/" \
 -e "/#define *SVN_VER_MINOR/s/[0-9]\+/$ver_minor/" \
 -e "/#define *SVN_VER_PATCH/s/[0-9]\+/$ver_patch/" \
 -e "/#define *SVN_VER_MICRO/s/[0-9]\+/$ver_patch/" \
 -e "/#define *SVN_VER_TAG/s/\".*\"/\" ($VER_TAG)\"/" \
 -e "/#define *SVN_VER_NUMTAG/s/\".*\"/\"$VER_NUMTAG\"/" \
 -e "/#define *SVN_VER_REVISION/s/[0-9]\+/$REVISION_SVN/" \
  < "$vsn_file" > "$vsn_file.tmp"

mv -f "$vsn_file.tmp" "$vsn_file"

cp "$vsn_file" "svn_version.h.dist"

echo "Running ./autogen.sh in sandbox, to create ./configure ..."
(cd "$DISTPATH" && ./autogen.sh --release) || exit 1

if [ ! -f $DISTPATH/neon/configure ]; then
  echo "Creating neon configure"
  (cd "$DISTPATH/neon" && ./autogen.sh) || exit 1
fi

echo "Removing any autom4te.cache directories that might exist..."
find "$DISTPATH" -depth -type d -name 'autom4te*.cache' -exec rm -rf {} \;

cat > "$DISTPATH/ChangeLog.CVS" <<EOF
The old CVS ChangeLog is kept at 

     http://subversion.tigris.org/

If you want to see changes since Subversion went self-hosting,
you probably want to use the "svn log" command -- and if it 
does not do what you need, please send in a patch!
EOF

if [ -z "$ZIP" ]; then
  # Do not use tar, it's probably GNU tar which produces tar files that are
  # not compliant with POSIX.1 when including filenames longer than 100 chars.
  # Platforms without a tar that understands the GNU tar extension will not
  # be able to extract the resulting tar file.  Use pax to produce POSIX.1
  # tar files.
  echo "Rolling $DISTNAME.tar ..."
  (cd "$DIST_SANDBOX" > /dev/null && pax -x ustar -w "$DISTNAME") > \
    "$DISTNAME.tar"

  echo "Compressing to $DISTNAME.tar.bz2 ..."
  bzip2 -9fk "$DISTNAME.tar"

  # Use the gzip -n flag - this prevents it from storing the original name of
  # the .tar file, and far more importantly, the mtime of the .tar file, in the
  # produced .tar.gz file. This is important, because it makes the gzip
  # encoding reproducable by anyone else who has an similar version of gzip,
  # and also uses "gzip -9n". This means that committers who want to GPG-sign
  # both the .tar.gz and the .tar.bz2 can download the .tar.bz2 (which is
  # smaller), and locally generate an exact duplicate of the official .tar.gz
  # file. This metadata is data on the temporary uncompressed tarball itself,
  # not any of its contents, so there will be no effect on end-users.
  echo "Compressing to $DISTNAME.tar.gz ..."
  gzip -9nf "$DISTNAME.tar"
else
  echo "Rolling $DISTNAME.zip ..."
  (cd "$DIST_SANDBOX" > /dev/null && zip -q -r - "$DISTNAME") > \
    "$DISTNAME.zip"
fi
echo "Removing sandbox..."
rm -rf "$DIST_SANDBOX"

sign_file()
{
  if [ -n "$SIGN" ]; then
    type gpg > /dev/null 2>&1
    if [ $? -eq 0 ]; then
      if test -n "$user"; then
        args="--default-key $user"
      fi
      for ARG in $@
      do
        gpg --armor $args --detach-sign $ARG
      done
    else
      type pgp > /dev/null 2>&1
      if [ $? -eq 0 ]; then
        if test -n "$user"; then
          args="-u $user"
        fi
        for ARG in $@
        do
          pgp -sba $ARG $args
        done
      fi
    fi
  fi
}

echo ""
echo "Done:"
if [ -z "$ZIP" ]; then
  ls -l "$DISTNAME.tar.bz2" "$DISTNAME.tar.gz"
  sign_file $DISTNAME.tar.gz $DISTNAME.tar.bz2
  echo ""
  echo "md5sums:"
  md5sum "$DISTNAME.tar.bz2" "$DISTNAME.tar.gz"
  type sha1sum > /dev/null 2>&1
  if [ $? -eq 0 ]; then
    echo ""
    echo "sha1sums:"
    sha1sum "$DISTNAME.tar.bz2" "$DISTNAME.tar.gz"
  fi
else
  ls -l "$DISTNAME.zip"
  sign_file $DISTNAME.zip
  echo ""
  echo "md5sum:"
  md5sum "$DISTNAME.zip"
  type sha1sum > /dev/null 2>&1
  if [ $? -eq 0 ]; then
    echo ""
    echo "sha1sum:"
    sha1sum "$DISTNAME.zip"
  fi
fi

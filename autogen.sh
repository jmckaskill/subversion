#!/bin/sh

### Run this to produce everything needed for configuration. ###

# Ensure some permissions for executables used by this script
for execfile in gen-make.py \
                dist.sh \
                ac-helpers/get-neon-ver.sh \
                ac-helpers/gnu-diff.sh \
                ac-helpers/gnu-patch.sh \
                ac-helpers/install.sh; do
  chmod +x $execfile
done


# Make sure the APR directory is present
if [ ! -d apr ]; then
  echo "You don't have an apr/ subdirectory here.  Please get one:"
  echo ""
  echo "   cvs -d :pserver:anoncvs@cvs.apache.org:/home/cvspublic login"
  echo "      (password 'anoncvs')"
  echo ""
  echo "   cvs -d :pserver:anoncvs@cvs.apache.org:/home/cvspublic co apr"
  echo ""
  echo "Run that right here in the top-level of the Subversion tree."
  echo ""
  PREREQ_FAILED="yes"
fi

# Make sure the Neon directory is present
NEON_WANTED=0.15.3
NEON_URL="http://www.webdav.org/neon/neon-${NEON_WANTED}.tar.gz"

if [ ! -d neon ]; then
  echo "You don't have a neon/ subdirectory here."
  echo "Please get neon ${NEON_WANTED} from:"
  echo "       ${NEON_URL}"
  echo ""
  echo "Unpack the archive using tar/gunzip and rename the resulting"
  echo "directory from ./neon-${NEON_WANTED}/ to ./neon/"
  echo ""
  PREREQ_FAILED="yes"
else
   NEON_VERSION=`ac-helpers/get-neon-ver.sh neon`
   if test "$NEON_WANTED" != "$NEON_VERSION"; then
     echo "You have a neon/ subdir containing version $NEON_VERSION,"
     echo "but Subversion needs neon ${NEON_WANTED}."
     echo "Please get neon ${NEON_WANTED} from:"
     echo "       ${NEON_URL}"
     echo ""
     echo "Unpack the archive using tar/gunzip and rename the resulting"
     echo "directory from ./neon-${NEON_WANTED}/ to ./neon/"
     echo ""
     PREREQ_FAILED="yes"
   fi
fi


#
# If PREREQ_FAILED == "yes", then one or more required packages could
# not be found in-tree, so exit now.
#
if [ "${PREREQ_FAILED}" = "yes" ]; then
  exit 1
fi


# Run a quick test to ensure APR is kosher.
(cd apr && build/buildcheck.sh) || exit 1




#
# Handle some libtool helper files
#
# ### eventually, we can/should toss this in favor of simply using
# ### APR's libtool. deferring to a second round of change...
#
echo "Copying libtool helper files..."

libtoolize=`apr/build/PrintPath glibtoolize libtoolize`
if [ "x$libtoolize" = "x" ]; then
    echo "libtoolize not found in path"
    exit 1
fi

$libtoolize --copy --automake

ltpath=`dirname $libtoolize`
ltfile=`cd $ltpath/../share/aclocal ; pwd`/libtool.m4

if [ ! -f $ltfile ]; then
    echo "$ltfile not found"
    exit 1
fi

rm -f ac-helpers/libtool.m4
cp $ltfile ac-helpers/libtool.m4

# This is just temporary until people's workspaces are cleared -- remove
# any old aclocal.m4 left over from prior build so it doesn't cause errors.
rm -f aclocal.m4

# Produce getdate.c from getdate.y.
# Again, this means that "developers" who run autogen.sh need either
# yacc or bison -- but not people who compile sourceballs, since `make
# dist` will include getdate.c.
echo "Creating getdate.c..."
bison -o subversion/libsvn_subr/getdate.c subversion/libsvn_subr/getdate.y
if [ $? -ne 0 ]; then
    yacc -o subversion/libsvn_subr/getdate.c subversion/libsvn_subr/getdate.y
    if [ $? -ne 0 ]; then
        echo
        echo "   Error:  can't find either bison or yacc."
        echo "   One of these is needed to generate the date parser."
        echo
        exit 1
    fi
fi

# Create the file detailing all of the build outputs for SVN.
#
# Note: this dependency on Python is fine: only SVN developers use autogen.sh
#       and we can state that dev people need Python on their machine
if test "$1" = "-s"; then
  echo "Creating build-outputs.mk (no dependencies)..."
  ./gen-make.py -s build.conf ;
else
  echo "Creating build-outputs.mk..."
  ./gen-make.py build.conf ;
fi

if test "$?" != "0"; then
  echo "gen-make.py failed, is python really installed?"
  exit 1
fi

# Produce config.h.in
# Do this before the automake (automake barfs if the header isn't available).
# Do it after the aclocal command -- automake sets up the header to depend
# on aclocal.m4
echo "Creating svn_private_config.h.in..."
autoheader

# If there's a config.cache file, we may need to delete it.
# If we have an existing configure script, save a copy for comparison.
if [ -f config.cache ] && [ -f configure ]; then
  cp configure configure.$$.tmp
fi

# Produce ./configure
echo "Creating configure..."
autoconf

# Meta-configure apr/ subdir
if [ -d apr ]; then
  echo "Creating config files for APR..."
  (cd apr; ./buildconf)  # this is apr's equivalent of autogen.sh
fi

# If we have a config.cache file, toss it if the configure script has
# changed, or if we just built it for the first time.
if [ -f config.cache ]; then
  (
    [ -f configure.$$.tmp ] && cmp configure configure.$$.tmp > /dev/null 2>&1
  ) || (
    echo "Tossing config.cache, since configure has changed."
    rm config.cache
  )
  rm -f configure.$$.tmp
fi

echo ""
echo "You can run ./configure now."
echo ""
echo "Running autogen.sh implies you are a maintainer.  You may prefer"
echo "to run configure in one of the following ways:"
echo ""
echo "./configure --enable-maintainer-mode"
echo "./configure --disable-shared"
echo "./configure --enable-maintainer-mode --disable-shared"
echo ""
echo "Note:  this build will create the Subversion shared libraries and a"
echo "       command-line client.  If you wish to build a Subversion server,"
echo "       you will need Apache 2.0.  See notes/dav_setup.txt for details."
echo ""

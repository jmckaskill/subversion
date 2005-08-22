#!/bin/sh

### Run this to produce everything needed for configuration. ###


# Run tests to ensure that our build requirements are met
RELEASE_MODE=""
SKIP_DEPS=""
while test $# != 0; do
  case "$1" in
    --release)
      RELEASE_MODE="$1"
      shift
      ;;
    -s)
      SKIP_DEPS="yes"
      shift
      ;;
    --)         # end of option parsing
      break
      ;;
    *)
      echo "invalid parameter: '$1'"
      exit 1
      ;;
  esac
done
# ### The order of parameters is important; buildcheck.sh depends on it and
# ### we don't want to copy the fancy option parsing loop there. For the
# ### same reason, all parameters should be quoted, so that buildcheck.sh
# ### sees an empty arg rather than missing one.
./build/buildcheck.sh "$RELEASE_MODE" || exit 1

### temporary cleanup during transition to libtool 1.4
(cd ac-helpers ; rm -f ltconfig ltmain.sh libtool.m4)

#
# Handle some libtool helper files
#
# ### eventually, we can/should toss this in favor of simply using
# ### APR's libtool. deferring to a second round of change...
#

libtoolize="`./build/PrintPath glibtoolize libtoolize libtoolize15`"

if [ "x$libtoolize" = "x" ]; then
    echo "libtoolize not found in path"
    exit 1
fi

$libtoolize --copy --automake

ltpath="`dirname $libtoolize`"
ltfile=${LIBTOOL_M4-`cd $ltpath/../share/aclocal ; pwd`/libtool.m4}

if [ ! -f $ltfile ]; then
    echo "$ltfile not found (try setting the LIBTOOL_M4 environment variable)"
    exit 1
fi

echo "Copying libtool helper: $ltfile"
cp $ltfile ac-helpers/libtool.m4

# Create the file detailing all of the build outputs for SVN.
#
# Note: this dependency on Python is fine: only SVN developers use autogen.sh
#       and we can state that dev people need Python on their machine. Note
#       that running gen-make.py requires Python 1.X or newer.

OK=`python -c 'print "OK"'`
if test "${OK}" != "OK" ; then
  echo "Python check failed, make sure python is installed and on the PATH"
  exit 1
fi

if test -n "$SKIP_DEPS"; then
  echo "Creating build-outputs.mk (no dependencies)..."
  python ./gen-make.py -s build.conf || gen_failed=1

  ### if apr and apr-util are not subdirs, then this fails. only do it
  ### for the release (from dist.sh; for now)
  if test -n "$RELEASE_MODE"; then
    echo "Creating MSVC files (no dependencies)..."
    python ./gen-make.py -t dsp -s build.conf || gen_failed=1
  fi
else
  echo "Creating build-outputs.mk..."
  python ./gen-make.py build.conf || gen_failed=1

  ### if apr and apr-util are not subdirs, then this fails. only do it
  ### for the release (from dist.sh; for now)
  if test -n "$RELEASE_MODE"; then
    echo "Creating MSVC files..."
    python ./gen-make.py -t dsp -s build.conf || gen_failed=1
  fi
fi

# Compile SWIG headers into standalone C files if we are in release mode
if test -n "$RELEASE_MODE"; then
  echo "Generating SWIG code..."
  echo abs_srcdir=. > autogen-standalone.mk
  cat build-outputs.mk >> autogen-standalone.mk
  make -f autogen-standalone.mk autogen-swig
  rm autogen-standalone.mk
fi

if test -n "$gen_failed"; then
  echo "ERROR: gen-make.py failed"
  exit 1
fi

# Produce config.h.in
echo "Creating svn_private_config.h.in..."
${AUTOHEADER:-autoheader}

# If there's a config.cache file, we may need to delete it.  
# If we have an existing configure script, save a copy for comparison.
if [ -f config.cache ] && [ -f configure ]; then
  cp configure configure.$$.tmp
fi

# Produce ./configure
echo "Creating configure..."
${AUTOCONF:-autoconf}

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

# Remove autoconf 2.5x's cache directory
rm -rf autom4te*.cache

# Run apr/buildconf if it exists.
if test -x "apr/buildconf" ; then
  echo "Creating configuration files for apr." # apr's equivalent of autogen.sh
  (cd apr && ./buildconf)
fi

# Run apr-util/buildconf if it exists.
if test -x "apr-util/buildconf" ; then
  echo "Creating configuration files for apr-util."
  (cd apr-util && ./buildconf)
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
echo "Note:  If you wish to run a Subversion HTTP server, you will need"
echo "Apache 2.0.  See the INSTALL file for details."
echo ""

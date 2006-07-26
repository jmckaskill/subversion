#!/bin/sh

### Run this to produce everything needed for configuration. ###


# Run tests to ensure that our build requirements are met
RELEASE_MODE=""
RELEASE_ARGS=""
SKIP_DEPS=""
while test $# != 0; do
  case "$1" in
    --release)
      RELEASE_MODE="$1"
      RELEASE_ARGS="--release"
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
cp $ltfile build/libtool.m4

# Create the file detailing all of the build outputs for SVN.
#
# Note: this dependency on Python is fine: only SVN developers use autogen.sh
#       and we can state that dev people need Python on their machine. Note
#       that running gen-make.py requires Python 2.X or newer.

PYTHON="`./build/find_python.sh`"
if test -z "$PYTHON"; then
  echo "Python 2.0 or later is required to run autogen.sh"
  echo "If you have a suitable Python installed, but not on the"
  echo "PATH, set the environment variable PYTHON to the full path"
  echo "to the Python executable, and re-run autogen.sh"
  exit 1
fi

# Compile SWIG headers into standalone C files if we are in release mode
if test -n "$RELEASE_MODE"; then
  echo "Generating SWIG code..."
  # Generate build-outputs.mk in non-release-mode, so that we can
  # build the SWIG-related files
  "$PYTHON" ./gen-make.py build.conf || gen_failed=1

  # Build the SWIG-related files
  make -f autogen-standalone.mk autogen-swig
fi

if test -n "$SKIP_DEPS"; then
  echo "Creating build-outputs.mk (no dependencies)..."
  "$PYTHON" ./gen-make.py $RELEASE_ARGS -s build.conf || gen_failed=1

  ### if apr and apr-util are not subdirs, then this fails. only do it
  ### for the release (from dist.sh; for now)
  if test -n "$RELEASE_MODE"; then
    echo "Creating MSVC files (no dependencies)..."
    "$PYTHON" ./gen-make.py $RELEASE_ARGS -t dsp -s build.conf || gen_failed=1
  fi
else
  echo "Creating build-outputs.mk..."
  "$PYTHON" ./gen-make.py $RELEASE_ARGS build.conf || gen_failed=1

  ### if apr and apr-util are not subdirs, then this fails. only do it
  ### for the release (from dist.sh; for now)
  if test -n "$RELEASE_MODE"; then
    echo "Creating MSVC files..."
    "$PYTHON" ./gen-make.py $RELEASE_ARGS -t dsp -s build.conf || gen_failed=1
  fi
fi

find build/ -name '*.pyc' -print -exec rm {} \;
rm autogen-standalone.mk

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
echo "Apache 2.x.  See the INSTALL file for details."
echo ""

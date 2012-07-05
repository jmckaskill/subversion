#!/bin/sh
# Copyright 2000-2004 The Apache Software Foundation
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#

# buildpkg.sh: This script builds a Solaris PKG from the source tree
#              provided.

PREFIX=/usr/local/
TEMPDIR=/var/tmp/$USER/subversion-root
rm -rf $TEMPDIR

apr_config=`which apr-config`
apu_config=`which apu-config`
neon_config=`which neon-config`
neon_config=`dirname $neon_config`
neon_config=`dirname $neon_config`
apxs=

while test $# -gt 0 
do
  # Normalize
  case "$1" in
  -*=*) optarg=`echo "$1" | sed 's/[-_a-zA-Z0-9]*=//'` ;;
  *) optarg= ;;
  esac

  case "$1" in
  --with-apr=*)
  apr_config=$optarg
  ;;
  esac

  case "$1" in
  --with-apr-util=*)
  apu_config=$optarg
  ;;
  esac

  case "$1" in
  --with-neon=*)
  neon_config=$optarg
  ;;
  esac

  case "$1" in
  --with-apxs=*)
  apxs="--with-apxs=$optarg"
  ;;
  esac

  case "$1" in
  --prefix=*)
  PREFIX=$optarg
  esac

  shift
done

if [ ! -f $apr_config -a ! -f $apr_config/configure.in ]; then
  echo "The apr source directory / apr-config could not be found"
  echo "Usage: buildpkg [--with-apr=[dir|file]] [--with-apr-util=[dir|file]]"
  exit 1
fi

if [ ! -f $apu_config -a ! -f $apu_config/configure.in ]; then
  echo "The apu source directory / apu-config could not be found"
  echo "Usage: buildpkg [--with-apr=[dir|file]] [--with-apr-util=[dir|file]]"
  exit 1
fi

./configure --prefix=$PREFIX \
            --with-apr=$apr_config \
            --with-apr-util=$apu_config \
            --with-neon=$neon_config \
            $apxs --with-ssl

make
make install DESTDIR=$TEMPDIR
. packages/solaris/pkginfo
cp packages/solaris/pkginfo $TEMPDIR$PREFIX

current=`pwd`
cd $TEMPDIR$PREFIX
echo "i pkginfo=./pkginfo" > prototype
find . -print | grep -v ./prototype | grep -v ./pkginfo | pkgproto | awk '{print $1" "$2" "$3" "$4" root bin"}' >> prototype
mkdir $TEMPDIR/pkg
pkgmk -r $TEMPDIR$PREFIX -d $TEMPDIR/pkg

cd $current
pkgtrans -s $TEMPDIR/pkg $current/$NAME-$VERSION-$ARCH-local
gzip $current/$NAME-$VERSION-$ARCH-local

rm -rf $TEMPDIR


#!/bin/sh

##########################################################################
# How to build a Subversion distribution tarball:
#
# Run this script in the top-level of a configured working copy that
# has apr/ and neon/ subdirs, and you'll end up with
# subversion-rXXX.tar.gz in that top-level directory.
#
# Unless specified otherwise (with a single REVISION argument to this
# script), the tarball will be based on the HEAD of the repository.
#
# It will *not* be based on whatever set of revisions are in your
# working copy.  However, since 
#
#   - the documentation will be produced by running "make doc" 
#     on your working copy's revisions of the doc master files, and
#
#   - since the APR tree included is basically copied from your working 
#     copy, 
#
# it's probably simplest if you just make sure your working copy is at
# the same revision as that of the distribution you are trying to
# create.  Then you won't get any unexpected results.
#
##########################################################################

### Rolling block.
DIST_SANDBOX=.dist_sandbox

### Estimated current version of your working copy
WC_VERSION=`svn st -vn doc/README | awk '{print $2}'`

### The "REV" part of ${DISTNAME}-rREV.tar.gz
if test "X$1" != X; then
  VERSION=$1
else
  VERSION=`svn st -vu README | tail -1 | awk '{print $3}'`
fi

### The tarball's basename, also the name of the subdirectory into which
### it should unpack.
DISTNAME=subversion-r${VERSION}
echo "Distribution will be named: ${DISTNAME}"

### Warn the user if their working copy looks to be out of sync with
### their requested (or default) revision
if test ${WC_VERSION} != ${VERSION}; then
  echo "* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *"
  echo "*                                                             *"
  echo "* WARNING:  The docs/ directory in your working copy does not *"
  echo "*           appear  to  have the same revision number  as the *"
  echo "*           distribution revision you requested.  Since these *"
  echo "*           documents will be the ones included in your final *"
  echo "*           tarball, please  be  sure they reflect the proper *"
  echo "*           state.                                            *"
  echo "*                                                             *"
  echo "* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *"
fi

### Clean up the old docs so we're guaranteed the latest ones.
# This is necessary only because "make clean" doesn't appear
# to clean up docs at the moment.
echo "Cleaning old docs in docs/ ..."
rm -f doc/programmer/design/svn-design.info
rm -f doc/programmer/design/svn-design.info-*
rm -f doc/programmer/design/svn-design.html
rm -f doc/programmer/design/svn-design.txt
rm -f doc/user/manual/svn-manual.info
rm -f doc/user/manual/svn-manual.html
rm -f doc/user/manual/svn-manual.txt

### Build new docs.
echo "Building new docs in docs/ ..."
make doc

### Prepare an area for constructing the dist tree.
rm -rf ${DIST_SANDBOX}
mkdir ${DIST_SANDBOX}
echo "Removed and recreated ${DIST_SANDBOX}"

### Export the dist tree, clean it up.
echo "Checking out revision ${VERSION} of Subversion into sandbox..."
(cd ${DIST_SANDBOX} && \
 svn co -r ${VERSION} http://svn.collab.net/repos/svn/trunk \
        -d ${DISTNAME} --username none --password none)
echo "Removing all .svn/ dirs from the checkout..."
rm -rf `find ${DIST_SANDBOX}/${DISTNAME} -name .svn -print`

### Ship with (relatively) clean APR and neon working copies
### inside the tarball, just to make people's lives easier.
echo "Copying apr into sandbox, making clean..."
cp -r apr ${DIST_SANDBOX}/${DISTNAME}
(cd ${DIST_SANDBOX}/${DISTNAME}/apr && make distclean)
# Defang the APR working copy.
echo "Removing all CVS/ and .cvsignore files from apr..."
rm -rf `find ${DIST_SANDBOX}/${DISTNAME}/apr -name CVS -type d -print`
rm -rf `find ${DIST_SANDBOX}/${DISTNAME}/apr -name .cvsignore -print`
# Clean most of neon.
echo "Coping neon into sandbox, making clean..."
cp -r neon ${DIST_SANDBOX}/${DISTNAME}
(cd ${DIST_SANDBOX}/${DISTNAME}/neon && make distclean)
# Then do some extra cleaning in neon, since its `make distclean'
# rule still leaves some .o files lying around.  Better to
# patch Neon, of course; but the fix wasn't obvious to me --
# something to do with @NEONOBJS@ in neon/src/Makefile.in?
echo "Cleaning *.o in neon..."
rm -f ${DIST_SANDBOX}/${DISTNAME}/neon/src/*.o

### Run autogen.sh in the dist, so we ship with a configure script.
# First make sure autogen.sh is executable, because, as Mike Pilato
# points out, until we get permission versioning working, it won't be
# executable on export from svn.
echo "Running ./autogen.sh in sandbox, to create ./configure ..."
chmod a+x ${DIST_SANDBOX}/${DISTNAME}/autogen.sh
(cd ${DIST_SANDBOX}/${DISTNAME} && ./autogen.sh)

### Copy all the pre-built docs, so we ship with ready documentation.
echo "Copying new docs into sandbox..."
for name in doc/programmer/design/svn-design.info   \
            doc/programmer/design/svn-design.info-* \
            doc/programmer/design/svn-design.html   \
            doc/programmer/design/svn-design.txt    \
            doc/user/manual/svn-manual.info         \
            doc/user/manual/svn-manual.html         \
            doc/user/manual/svn-manual.txt
do
   cp ${name} ${DIST_SANDBOX}/${DISTNAME}/${name}
done

### Tell people where to find old information.
cat > ${DIST_SANDBOX}/${DISTNAME}/ChangeLog.CVS <<EOF
The old CVS ChangeLog is kept at 

     http://subversion.tigris.org

If you want to see changes since Subversion went self-hosting,
you probably want to use the "svn log" command -- and if it 
does not do what you need, please send in a patch!
EOF

### Make the tarball.
echo "Rolling ${DISTNAME}.tar.gz ..."
(cd ${DIST_SANDBOX} && tar zcvpf ${DISTNAME}.tar.gz ${DISTNAME})

### Copy it upstairs and clean up.
echo "Copying tarball out, removing sandbox..."
cp ${DIST_SANDBOX}/${DISTNAME}.tar.gz .
rm -rf ${DIST_SANDBOX}

echo ""
echo "Done:"
ls -l ${DISTNAME}.tar.gz
echo ""

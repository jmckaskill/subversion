%define apache_version 2.0.48-0.1
%define apr_version 0.9.5
%define neon_version 0.24.7
%define swig_version 1.3.19
%define apache_dir /usr
# If you don't have 360+ MB of free disk space or don't want to run checks then
# set make_*_check to 0.
%define make_ra_local_check 1
%define make_ra_svn_check 1
%define make_ra_dav_check 1
Summary: A Concurrent Versioning system similar to but better than CVS.
Name: subversion
Version: @VERSION@
Release: @RELEASE@
Copyright: BSD
Group: Utilities/System
URL: http://subversion.tigris.org
SOURCE0: subversion-%{version}-%{release}.tar.gz
SOURCE3: filter-requires.sh
Vendor: Summersoft
Packager: David Summers <david@summersoft.fay.ar.us>
Requires: apr >= %{apr_version}
Requires: apr-util >= %{apr_version}
Requires: db42 >= 4.2.52
Requires: neon >= %{neon_version}
BuildPreReq: autoconf >= 2.53
BuildPreReq: db42-devel >= 4.2.52
BuildPreReq: docbook-style-xsl >= 1.58.1
BuildPreReq: doxygen
BuildPreReq: expat-devel
BuildPreReq: httpd >= %{apache_version}
BuildPreReq: apr-devel >= %{apr_version}
BuildPreReq: apr-util-devel >= %{apr_version}
BuildPreReq: libtool >= 1.4.2
BuildPreReq: libxslt >= 1.0.27
BuildPreReq: neon-devel >= %{neon_version}
BuildPreReq: openssl-devel
BuildPreReq: perl
BuildPreReq: python
BuildPreReq: python-devel
BuildPreReq: swig >= %{swig_version}
BuildPreReq: zlib-devel
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}
Prefix: /usr
%description
Subversion is a concurrent version control system which enables one or more
users to collaborate in developing and maintaining a hierarchy of files and
directories while keeping a history of all changes.  Subversion only stores
the differences between versions, instead of every complete file.  Subversion
also keeps a log of who, when, and why changes occurred.

As such it basically does the same thing CVS does (Concurrent Versioning System)
but has major enhancements compared to CVS and fixes a lot of the annoyances
that CVS users face.

*** Note: This is a relocatable package; it can be installed anywhere you like
with the "rpm -Uvh --prefix /your/favorite/path" command. This is useful
if you don't have root access on your machine but would like to use this
package.

%package devel
Group: Utilities/System
Summary: Development package for Subversion developers.
Requires: subversion = %{version}-%{release}
%description devel
The subversion-devel package includes the static libraries and include files
for developers interacting with the subversion package.

%package server
Group: Utilities/System
Summary: Apache server module for Subversion server.
Requires: apr >= %{apr_version}
Requires: apr-util >= %{apr_version}
Requires: subversion = %{version}-%{release}
Requires: httpd >= %{apache_version}
BuildPreReq: httpd-devel >= %{apache_version}
%description server
The subversion-server package adds the Subversion server Apache module to
the Apache directories and configuration.

%package perl
Group: Utilities/System
Summary: Allows Perl scripts to directly use Subversion repositories.
Requires: swig >= %{swig_version}
Requires: perl
%description perl
Provides Perl (SWIG) support for Subversion.

%package python
Group: Utilities/System
Summary: Allows Python scripts to directly use Subversion repositories.
Requires: swig >= %{swig_version}
Requires: python >= 2
%description python
Provides Python (SWIG) support for Subversion.

%package tools
Group: Utilities/System
Summary: Tools for Subversion
%description tools
Tools for Subversion.

%changelog
* Sun Mar 27 2005 David Summers <david@summersoft.fay.ar.us> r13711
- Take out "static build" feature that never actually worked as intended.

* Sun Mar 27 2005 David Summers <david@summersoft.fay.ar.us> r13709
- Fix http tests to work with new locking feature which now requires
  authentication.

* Sun Jan 09 2005 David Summers <david@summersoft.fay.ar.us> r13417
- Supplementary: Take out documentation patch altogether.
- Turn testing back on, it was accidentally turned off.

* Sun Jan 09 2005 David Summers <david@summersoft.fay.ar.us> r13132
- Bye bye book;  it is now no longer a part of the Subversion repository but
  is at the http://svn.red-bean.com/svnbook/ URL.
  I will probably create a separate RPM package for it now...stay tuned.

* Sun Jan 09 2005 David Summers <david@summersoft.fay.ar.us> 1.1.2-12650
- Delete apr, apr-util, and neon from the distribution tree as those
  packages are already installed.

* Wed Dec 29 2004 David Summers <david@summersoft.fay.ar.us> 1.1.2-12531
- Added "noreplace" option to subversion.conf to not replace it if it has
  been changed.  This (hopefully) prevents the server from failing when doing
  an upgrade.  Thanks to Peter Holzleitner for the suggestion.

* Wed Jul 07 2004 David Summers <david@summersoft.fay.ar.us> 1.1.0-10174
- Require neon-0.24.7 to fix invalid XML (compression) bug.

* Wed May 05 2004 David Summers <david@summersoft.fay.ar.us> 1.1.0-9628
- Track changes to perl binding compile and install.
- Locale files now installed.

* Wed Mar 10 2004 David Summers <david@summersoft.fay.ar.us> 1.0.0-8983
- Fedora-1 and WBEL-3 require db42-4.2.52 or greater.

* Tue Feb 24 2004 David Summers <david@summersoft.fay.ar.us> 1.0.0-8823
- cvs2svn moved to separate project/package.

* Thu Feb 19 2004 David Summers <david@summersoft.fay.ar.us> 0.37.0-8770
- cvs2svn.1 man page taken out of distribution.  Change RPM so that if it
  just so happens to magically reappear someday then it will get put into
  the package.

* Wed Jan 28 2004 David Summers <david@summersoft.fay.ar.us> 0.37.0-8534
- Change version number to new format based on dev list discussion.

* Sun Jan 18 2004 David Summers <david@summersoft.fay.ar.us> 0.36.0-8372
- Switched to the Redhat way of doing the "swig" package where it is not
  separated into "swig" and "swig-runtime".
- Added subversion-perl package to support Perl (SWIG) bindings.

* Sat Jan 17 2004 David Summers <david@summersoft.fay.ar.us> 0.36.0-8362
- Now requires swig-1.3.19 so we can build the upcoming perl bindings.

* Thu Dec 25 2003 David Summers <david@summersoft.fay.ar.us> 0.35.1-8085
- Patch by Ben Reser <ben@reser.org> to get documentation to build again.
- svnserve.conf and svnserve manual pages added.

* Fri Dec 19 2003 David Summers <david@summersoft.fay.ar.us> 0.35.0-8054
- Require httpd-2.0.48.  *** Note: I don't yet have packages for httpd-2.0.48.
- Require apr-0.9.5.

* Tue Oct 25 2003 Blair Zajac <blair@orcaware.com> 0.32.1-7520
- Require neon 0.24.4.

* Tue Oct  7 2003 Blair Zajac <blair@orcaware.com> 0.30.0-7342
- Require neon 0.24.3.

* Sat Jul 19 2003 David Summers <david@summersoft.fay.ar.us> 0.25.0-6515
- Added cvs2svn regression tests.

* Sat Jul 19 2003 David Summers <david@summersoft.fay.ar.us> 0.25.0-6514
- PORTING file no longer exists.
- Thanks to Ralph Loader <suckfish@ihug.co.nz> for the following changes:
- Get rid of extraneous perl module dependencies via filter-requires.sh script.
- gdbm-devel is not a build pre-req for subversion but APR/APR-UTIL.
- LDFLAGS no longer needed when compiling without subversion-devel installed.
- Use %configure instead of ./configure.
- expat is not a direct dependency of subversion.
- No need to copy separate subversion.conf and httpd.davcheck.conf as they
  are in the source tree, just reference them there.
- Simplify "make install" by making use of DESTDIR macro instead of setting
  individual directory components.

* Sun Jul 13 2003 David Summers <david@summersoft.fay.ar.us> 0.25.0-6462
- Fix revision number to be properly generated during RPM build.  Can't use
  the normal svnversion command at this point because the SRPM is not a
  repository that we can get the version from via svnversion command.

* Sun Jul 13 2003 David Summers <david@summersoft.fay.ar.us> 0.25.0-6461
- Fix install/un-install not to bomb out on update if apache (httpd) doesn't
  restart correctly.

* Thu Jul 10 2003 David Summers <david@summersoft.fay.ar.us> 0.25.0-6434
- Apache 2.0.47 now recommended because of security issues.

* Sat Jun 28 2003 David Summers <david@summersoft.fay.ar.us> 0.24.2-6365
- RedHat decided to break out apr and apr-util separately from apache
  (httpd package).
- This also now builds on RedHat 9.0 because the new apr/apu-config correctly
  picks up the openssl include files.

* Tue Jun 24 2003 David Summers <david@summersoft.fay.ar.us> 0.24.2-6334
- Now requires apache 2.0.46.

* Mon Jun 16 2003 David Summers <david@summersoft.fay.ar.us> 0.24.1-6256
- Added doxygen documentation.

* Tue Jun 10 2003 David Summers <david@summersoft.fay.ar.us> 0.23.0-6188
- Track changes for addition of mod_authz_svn httpd module.

* Sat Jun 07 2003 David Summers <david@summersoft.fay.ar.us> 0.23.0-6163
- svn-design.info is no longer built.

* Sat May 24 2003 David Summers <david@summersoft.fay.ar.us> 0.23.0-6036
- Track changes to Python SWIG build.
- Now requires neon-0.23.9 to pick up bug and security fixes.
- Now builds the book and puts it in /usr/share/doc/subversion-VERSION/book
  directory.  RedHat 7.x and RedHat 8.x users who build this RPM will need to
  install or upgrade to the RedHat 9.0 docbook-style-xsl and libxslt packages.

* Thu May 15 2003 David Summers <david@summersoft.fay.ar.us> 0.22.2-5943
- The subversion package now requires python 2 because cvs2svn has been
  unswigified and just depends on python 2.
- The new subversion-python package requires python 2.

* Sat May 10 2003 David Summers <david@summersoft.fay.ar.us> 0.22.1-5879
- svn-config has been taken back out of the distribution.
- cvs2svn no longer requires SWIG, so rename the subversion-cvs2svn package to 
  subversion-python and move the cvs2svn and RCS parser into the subversion
  package.
- Added cvs2svn man page.

* Sun Apr 13 2003 David Summers <david@summersoft.fay.ar.us> 0.20.1-5610
- Added svndumpfilter.

* Fri Apr 04 2003 David Summers <david@summersoft.fay.ar.us> 0.18.1-5549
- Updated to Apache 2.0.45.
- Took out libsvn_auth as it is no longer needed or used.

* Sat Mar 01 2003 David Summers <david@summersoft.fay.ar.us> 0.18.1-5173
- Enabled RA_DAV checking.
  Now requires httpd package to build because of RA_DAV tests.

* Sat Jan 18 2003 David Summers <david@summersoft.fay.ar.us> 0.16.1-4433
- Created tools package to hold the tools.

* Thu Jan 16 2003 David Summers <david@summersoft.fay.ar.us> 0.16.1-4405
- Now requires httpd >= 2.0.44-0.1 (APACHE_2_0_BRANCH) which contains the new
  version of APR/APR-UTILS as of 2003.01.15.
- Added svnversion command.

* Tue Dec 31 2002 David Summers <david@summersoft.fay.ar.us> 0.16.0-4218
- Create a svnadmin.static which is copied to svnadmin-version-release
  when the package is erased, so users can still dump/load their repositories
  even after they have upgraded the RPM package.

* Sat Dec 14 2002 David Summers <david@summersoft.fay.ar.us> 0.16.0-4128
- SWIG now builds so we can use cvs2svn.

* Fri Oct 04 2002 David Summers <david@summersoft.fay.ar.us> 0.14.3-3280
- Made cvs2svn conditional (at least until we can get it to build consistently
  and work).

* Sat Sep 21 2002 David Summers <david@summersoft.fay.ar.us> 0.14.3-3205
- Added SWIG dependencies to add cvs2svn capabilities.

* Fri Aug 16 2002 David Summers <david@summersoft.fay.ar.us> 0.14.1-2984
- Now requires neon-0.22.0.

* Thu Aug 15 2002 David Summers <david@summersoft.fay.ar.us> 0.14.1-2978
- Took out loading mod_dav_svn from subversion.spec file and put it in
  subversion.conf file which goes into the apache conf directory.
- Simplify what gets put into httpd.conf to only the include for the
  subversion.conf file.
  (Thanks to Scott Harrison <sharrison@users.sourceforge.net> for prompting
  me to do this).

* Thu Aug 08 2002 David Summers <david@summersoft.fay.ar.us> 0.14.0-2919
- Updated to APR/APR-UTIL 2002-08-08.

* Tue Jun 25 2002 David Summers <david@summersoft.fay.ar.us> 0.13.0-2332
- Updated to APACHE/APR/APR-UTIL 2002-06-25.
- Previous version had a few problems because of missing apache error/ files.

* Sun Jun 23 2002 David Summers <david@summersoft.fay.ar.us> 0.13.0-2318
- Updated to apache-2.0.40-0.3.
- Updated to subversion-0.13.1-2318.

* Tue Jun 18 2002 David Summers <david@summersoft.fay.ar.us> 0.13.0-2277
- Updated for RedHat 7.3 (autoconf253).
- Added a bunch of pre-requisites I didn't know were needed because I built a
  new machine that didn't have them already installed.
- Fixed installation of man and info documentation pages.

* Wed Mar 06 2002 David Summers <david@summersoft.fay.ar.us> 0.9.0-1447
- Back to apache-libapr* stuff, hopefully to stay.

* Sun Feb 24 2002 David Summers <david@summersoft.fay.ar.us> 0.9.0-1373
- Fixed expat.patch to not have to make so many changes by writing a small
  shell script that changes libexpat to -lexpat.

* Fri Feb 22 2002 Blair Zajac <blair@orcaware.com> 0.9.0-1364
- Updated to neon-0.19.2.

* Mon Feb 11 2002 David Summers <david@summersoft.fay.ar.us> 0.8.0-1250
- Back to using apr and apr-util separately from apache.

* Mon Feb 11 2002 David Summers <david@summersoft.fay.ar.us> 0.8.0-1232
- Updated to APR and APR-UTIL 2002.02.11.
- Updated to apache-2.0.32-0.2. (Requires apache-libapr and apache-libapr-util).
- Took out a (now non-existant) documentation file.
- Moved SPEC file changelog to after all package definitions.
  
* Sun Feb 03 2002 David Summers <david@summersoft.fay.ar.us> 0.8.0-1153
- Updated to neon-0.18.5.
- Broke up apache and apache-devel into apache-apr, apache-apr-devel,
  apache-apr-utils, and apache-apr-utils-devel.
- Updated apache to APR and APR-UTILS to 2002.02.03 version.

* Sat Feb 02 2002 David Summers <david@summersoft.fay.ar.us> 0.8.0-1147
- Now builds without the separate APR package as it is built into and
  "exported" from apache-2.0.31-0.3.

* Fri Feb 01 2002 David Summers <david@summersoft.fay.ar.us> 0.8.0-1132
- Took out patches to install procedure now not required because of fixes
  in rev 1130.

* Fri Feb 01 2002 David Summers <david@summersoft.fay.ar.us> 0.8.0-1129
- Added requirement for APR 0.2002.01.19 rev 2 where the /usr/bin/apr-config
  program was added.

* Sun Oct 28 2001 David Summers <david@summersoft.fay.ar.us>
- Release M5-r340: Added the subversion-server package.

* Fri Oct 26 2001 David Summers <david@summersoft.fay.ar.us>
- Release M5-r327: No longer need expat-lite. We can use the normal expat.

* Thu Sep 27 2001 David Summers <david@summersoft.fay.ar.us>
- Release M3-r117: Initial Version.

%define __perl_requires %{SOURCE3}
%define perl_vendorarch %(eval "`perl -V:installvendorarch`"; echo $installvendorarch)
%define perl_version %(eval "`perl -V:version`"; echo $version)

%prep
%setup -q

if [ -f /usr/bin/autoconf-2.53 ]; then
   AUTOCONF="autoconf-2.53"
   AUTOHEADER="autoheader-2.53"
   export AUTOCONF AUTOHEADER
fi
sh autogen.sh


# Fix up mod_dav_svn installation.
patch -p1 < packages/rpm/wbel-3/install.patch

# Figure out version and release number for command and documentation display.
case "%{release}" in
   1)
      # Build an official release
      RELEASE_NAME="%{version}"
      ;;
   alpha*|beta*|gamma*)
      # Build an alpha, beta, gamma release.
      RELEASE_NAME="%{version} (%{release})"
      ;;
   *)
      # Build a working copy release
      RELEASE_NAME="%{version} (dev build, r%{release})"
      ;;
esac

# Delete apr, apr-util, and neon from the tree as those packages should already
# be installed.
rm -rf apr apr-util neon


%configure \
	--with-swig \
	--with-python=/usr/bin/python2.2 \
	--with-apxs=%{apache_dir}/sbin/apxs \
	--with-apr=%{apache_dir}/bin/apr-config \
	--with-apr-util=%{apache_dir}/bin/apu-config

%build
make clean
make

# Build python bindings
make swig-py

# Build PERL bindings
make swig-pl-lib
cd subversion/bindings/swig/perl/native
env APR_CONFIG=/usr/bin/apr-config perl Makefile.PL INSTALLDIRS=vendor PREFIX=$RPM_BUILD_ROOT/%{_prefix}
make all
make test
cd ../../../../..

%if %{make_ra_local_check}
echo "*** Running regression tests on RA_LOCAL (FILE SYSTEM) layer ***"
make check
echo "*** Finished regression tests on RA_LOCAL (FILE SYSTEM) layer ***"
%endif

%if %{make_ra_svn_check}
echo "*** Running regression tests on RA_SVN (SVN method) layer ***"
killall lt-svnserve || true
sleep 1
./subversion/svnserve/svnserve -d -r `pwd`/subversion/tests/clients/cmdline/
make svncheck
killall lt-svnserve
echo "*** Finished regression tests on RA_SVN (SVN method) layer ***"
%endif

%if %{make_ra_dav_check}
echo "*** Running regression tests on RA_DAV (HTTP method) layer ***"
killall httpd || true
sleep 1
sed -e "s;@SVNDIR@;`pwd`;" < packages/rpm/wbel-3/httpd.davcheck.conf > httpd.conf
cat > passwd <<EOF
jrandom:xCGl35kV9oWCY
jconstant:xCGl35kV9oWCY
EOF
/usr/sbin/httpd -f `pwd`/httpd.conf
sleep 1
make check BASE_URL='http://localhost:15835'
killall httpd
echo "*** Finished regression tests on RA_DAV (HTTP method) layer ***"
%endif

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/%{apache_dir}/conf
make install DESTDIR="$RPM_BUILD_ROOT"

# Add subversion.conf configuration file into httpd/conf.d directory.
mkdir -p $RPM_BUILD_ROOT/etc/httpd/conf.d
cp packages/rpm/wbel-3/subversion.conf $RPM_BUILD_ROOT/etc/httpd/conf.d

# Install Python SWIG bindings.
make install-swig-py DESTDIR=$RPM_BUILD_ROOT DISTUTIL_PARAM=--prefix=$RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/usr/lib/python2.2/site-packages
mv $RPM_BUILD_ROOT/usr/lib/svn-python/* $RPM_BUILD_ROOT/usr/lib/python2.2/site-packages
rmdir $RPM_BUILD_ROOT/usr/lib/svn-python

# Install PERL SWIG bindings.
make install-swig-pl-lib DESTDIR=$RPM_BUILD_ROOT
cd subversion/bindings/swig/perl/native
make PREFIX=$RPM_BUILD_ROOT/%{_prefix} install
cd ../../../../..

# Clean up unneeded files for package installation
rm -rf $RPM_BUILD_ROOT/%{_prefix}/lib/perl5/%{perl_version}

# Set up tools package files.
mkdir -p $RPM_BUILD_ROOT/usr/lib/subversion
cp -r tools $RPM_BUILD_ROOT/usr/lib/subversion

# Create doxygen documentation.
doxygen doc/doxygen.conf

%post server
# Restart apache server if needed.
source /etc/init.d/functions
if [ "`pidof httpd`"x != "x" ]; then
   /etc/init.d/httpd restart || true
fi

%postun server
# Restart apache server if needed.
source /etc/init.d/functions
if [ "`pidof httpd`"x != "x" ]; then
   /etc/init.d/httpd restart || true
fi

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc BUGS CHANGES COMMITTERS COPYING HACKING INSTALL README
%doc subversion/LICENSE
/usr/bin/svn
/usr/bin/svnadmin
/usr/bin/svndumpfilter
/usr/bin/svnlook
/usr/bin/svnserve
/usr/bin/svnversion
/usr/lib/libsvn_client*so*
/usr/lib/libsvn_delta*so*
/usr/lib/libsvn_diff*so*
/usr/lib/libsvn_fs*so*
/usr/lib/libsvn_ra*so*
/usr/lib/libsvn_repos*so*
/usr/lib/libsvn_subr*so*
/usr/lib/libsvn_wc*so*
/usr/share/locale/*/*/*
/usr/share/man/man1/*
/usr/share/man/man5/*
/usr/share/man/man8/*

%files devel
%defattr(-,root,root)
%doc doc/doxygen/html/*
/usr/lib/libsvn*.a
/usr/lib/libsvn*.la
/usr/include/subversion-1

%files server
%defattr(-,root,root)
%config(noreplace) /etc/httpd/conf.d/subversion.conf
%{apache_dir}/lib/httpd/modules/mod_dav_svn.la
%{apache_dir}/lib/httpd/modules/mod_dav_svn.so
%{apache_dir}/lib/httpd/modules/mod_authz_svn.la
%{apache_dir}/lib/httpd/modules/mod_authz_svn.so

%files perl
%defattr(-,root,root)
%{perl_vendorarch}/SVN
%{perl_vendorarch}/auto/SVN
/usr/lib/libsvn_swig_perl*so*
/usr/share/man/man3/SVN*

%files python
%defattr(-,root,root)
/usr/lib/python2.2/site-packages/svn
/usr/lib/python2.2/site-packages/libsvn
/usr/lib/libsvn_swig_py*so*

%files tools
%defattr(-,root,root)
/usr/lib/subversion/tools

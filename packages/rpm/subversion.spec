%define apache_version 2.0.32-0.2
%define neon_version 0.18.5
%define apr_date 2002.02.11
Summary: A Concurrent Versioning system similar to but better than CVS.
Name: subversion
Version: @VERSION@
Release: @RELEASE@
Copyright: BSD
Group: Utilities/System
URL: http://subversion.tigris.org
Source0: subversion-%{version}-%{release}.tar.gz
Patch0: expat.patch
Patch1: install.patch
Vendor: Summersoft
Packager: David Summers <david@summersoft.fay.ar.us>
Requires: apr >= 0.%{apr_date}
Requires: apr-util >= 0.%{apr_date}
Requires: db >= 4.0.14
Requires: expat
Requires: neon = %{neon_version}
Requires: /sbin/install-info
BuildPreReq: apache-devel >= %{apache_version}
BuildPreReq: apr-devel >= 0.%{apr_date}
BuildPreReq: apr-util-devel >= 0.%{apr_date}
BuildPreReq: autoconf >= 2.52
BuildPreReq: db-devel >= 4.0.14
BuildPreReq: expat-devel
BuildPreReq: libtool >= 1.4.2
BuildPreReq: neon = %{neon_version}
BuildPreReq: python
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}
Prefix: /usr
%description
Subversion does the same thing CVS does (Concurrent Versioning System) but has
major enhancements compared to CVS.

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
for developers interacing with the subversion package.

%package server
Group: Utilities/System
Summary: Apache server module for Subversion server.
Requires: apr >= 0.%{apr_date}
Requires: apr-util >= 0.%{apr_date}
Requires: perl
Requires: subversion = %{version}-%{release}
BuildPreReq: apache-devel >= %{apache_version}
%description server
The subversion-server package adds the Subversion server Apache module to
the Apache directories and configuration.

%changelog
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

%prep
%setup -q

# Fix up expat library.
%patch0 -p1

sh autogen.sh

# EXPAT is external so get rid of all except (patched) xmlparse.h
rm -rf expat-lite/[a-w]*.[ch]
rm -rf expat-lite/xmldef.h
rm -rf expat-lite/xmlparse.c
rm -rf expat-lite/xmlrole*
rm -rf expat-lite/xmltok*

LDFLAGS="-lcrypt -ldl -pthread" ./configure --prefix=/usr --with-apr=/usr --with-apr-util=/usr

# Fix up mod_dav_svn installation.
%patch1 -p1

%build
make

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/usr/share
make prefix=$RPM_BUILD_ROOT/usr libexecdir=$RPM_BUILD_ROOT/usr/lib/apache install

# Install man page until the previous install can do it correctly.
mv $RPM_BUILD_ROOT/usr/man $RPM_BUILD_ROOT/usr/share/man

# Install INFO pages in correct place.
mv $RPM_BUILD_ROOT/usr/info $RPM_BUILD_ROOT/usr/share/info

%post
/sbin/install-info /usr/share/info/svn-design.info.gz /usr/share/info/dir --entry='* Subversion-design: (svn-design).          Subversion Versioning System Design Manual'

/sbin/install-info /usr/share/info/svn-manual.info.gz /usr/share/info/dir --entry='* Subversion: (svn-manual).          Subversion Versioning System Manual'

/sbin/install-info /usr/share/info/svn_for_cvs_users.info.gz /usr/share/info/dir --entry='* Subversion-cvs: (svn_for_cvs_users).          Subversion Versioning System Information for CVS Users'

%preun
/sbin/install-info --delete /usr/share/info/svn-design.info.gz /usr/share/info/dir --entry='* Subversion-design: (svn-design).          Subversion Versioning System Design Manual'

/sbin/install-info --delete /usr/share/info/svn-manual.info.gz /usr/share/info/dir --entry='* Subversion: (svn-manual).          Subversion Versioning System Manual'

/sbin/install-info --delete /usr/share/info/svn_for_cvs_users.info.gz /usr/share/info/dir --entry='* Subversion-cvs: (svn_for_cvs_users).          Subversion Versioning System Information for CVS Users'


%post server
# Load subversion server into apache configuration.
CONF=/etc/httpd/conf/httpd.conf

# Search for Subversion dav_svn_module and add it to config file if not found.

if [ "`grep -i dav_svn_module $CONF`"x = "x" ]; then
   # Put in LoadModule dav_svn_module line at end of LoadModule section.
   perl -e '
   while ( <> )
      {
      $FirstLoadFound = 1 if ( ! $FirstLoadFound &&
           (/^LoadModule/ || /^#LoadModule/ ||  /^# LoadModule/) );
      $InsertPointFound = 1,
         print "LoadModule dav_svn_module modules/libmod_dav_svn.so\n"
         if ( $FirstLoadFound && ! $InsertPointFound &&
              ! (/^LoadModule/ || /^#LoadModule/ || /^# LoadModule/ ) );
      print;
      }
   ' < $CONF > $CONF.new && mv $CONF $CONF.bak && mv $CONF.new $CONF
fi

# Conditionally add subversion example configuration.
if [ "`grep -i svnpath $CONF`"x = "x" ]; then
   cat >> $CONF <<EOF

# Begin Subversion server configuration - Please don't delete this line.
#<Location /svn/repos>
#   DAV svn
#   SVNPath /home/svnroot
#
#   # Limit write permission to list of valid users.
#   <LimitExcept GET PROPFIND OPTIONS REPORT>
#      # Require SSL connection for password protection.
#      # SSLRequireSSL
#
#      AuthType Basic
#      AuthName "Authorization Realm"
#      AuthUserFile /absolute/path/to/passwdfile
#      Require valid-user
#   </LimitExcept>
#</Location>
# End Subversion server configuration - Please don't delete this line.
EOF
fi

# Restart apache server if needed.
source /etc/init.d/functions
if [ "`pidof httpd`"x != "x" ]; then
   /etc/init.d/httpd restart
fi

%preun server
# Take subversion configuration out of apache configuration file.
# Only take it out if this package is being erased and not upgraded.
if [ "$1" = "0" ];
   then
   cd /etc/httpd/conf && sed -e 's/^LoadModule dav_svn_module/#LoadModule dav_svn_module/' -e '/^# Begin Subversion server/,/^# End Subversion server/s/^/#/' < httpd.conf > httpd.conf.new && mv httpd.conf httpd.conf.bak && mv httpd.conf.new httpd.conf
fi

%postun server
# Restart apache server if needed.
source /etc/init.d/functions
if [ "`pidof httpd`"x != "x" ]; then
   /etc/init.d/httpd restart
fi

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc BUGS COMMITTERS COPYING HACKING IDEAS INSTALL PORTING README
%doc tools subversion/LICENSE
/usr/bin/svn
/usr/bin/svnadmin
/usr/bin/svnlook
/usr/lib/libsvn*so*
/usr/share/man/man1/*
/usr/share/info/*

%files devel
%defattr(-,root,root)
/usr/lib/libsvn*.a
/usr/lib/libsvn*.la
/usr/include/svn*

%files server
%defattr(-,root,root)
/usr/lib/apache/libmod_dav_svn.*

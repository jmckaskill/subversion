#!/bin/sh

# Subversion po file translation status report generator

# This file is based on the GNU gettext msgattrib tool
#  for message file filtering

# To make the script work, make sure that:

# 1) the script knows where to find msgattrib
# 2) you have checked out the required revision
# 2) you have run autogen.sh and configure for that revision
# 3) you have run 'make locale-gnu-po-update'


BIN=/bin
USRBIN=/usr/bin

DIRNAME=$USRBIN/dirname
GREP=$BIN/grep
MAKE=$USRBIN/make
RM=$BIN/rm
SED=$BIN/sed
MSGATTRIB=/usr/local/bin/msgattrib
MSGFMT=/usr/local/bin/msgfmt


SVNDIR=/usr/local/bin
SENDMAIL=/usr/sbin/sendmail
SVN=$SVNDIR/svn
SVNVERSION=$SVNDIR/svnversion
REVISION_PREFIX='r'



EXEC_PATH=`$DIRNAME "$0"`
WC_L='/usr/bin/wc -l'

cd $EXEC_PATH/../..

root_path="$PWD"
ROOT_PARENT_PATH="`$DIRNAME $root_path`"
branch_name="`echo $root_path | $SED -e "s@$ROOT_PARENT_PATH/@@"`"

wc_version=`$SVNVERSION subversion/po | $SED -e 's/[MS]//g'`
cd subversion/po

echo "

Translation status report for revision $wc_version ($branch_name/)

============================================================================"


for i in *.po ; do
  translated=`$MSGATTRIB --translated $i | $GREP -E '^msgid *"' | $SED -n '2~1p' | $WC_L`
  untranslated=`$MSGATTRIB --untranslated $i | $GREP -E '^msgid *"' | $SED -n '2~1p' | $WC_L`
  fuzzy=`$MSGATTRIB --only-fuzzy $i | $GREP -E '^msgid *"' | $SED -n '2~1p' | $WC_L`
  obsolete=`$MSGATTRIB --only-obsolete $i | $GREP -E '^msgid *"' | $SED -n '2~1p' | $WC_L`

  echo
  if test -z "`$SVN status $i | $GREP -E '^\?'`" ; then
      echo "Status for '$i': in repository"
  else
      echo "Status for '$i': NOT in repository"
      echo " (See the issue tracker 'translations' subcomponent)"
  fi

  echo
  if ! $MSGFMT --check-format -o /dev/null $i ; then
      echo "   FAILS GNU msgfmt --check-format"
  else
      echo "   Passes GNU msgfmt --check-format"
      echo
      echo "   Statistics:"
      echo "    $obsolete obsolete"
      echo "    $untranslated untranslated"
      echo "    $translated translated, of which"
      echo "       $fuzzy fuzzy"
  fi
  echo "
----------------------------------------------------------------------------"
done


#!/bin/sh

#
# Root of the test tree
#
TEST_ROOT="/home/brane/svn"

# Installation path, everything under that is considered 
# to be temporary
INST_DIR="$TEST_ROOT/inst"

#
# Repository paths and projects name
#
# installation paths are expected to be:
# '$INST_DIR/<proj_name>, so take care of your
# $CONFIG_PREFIX.<proj_name> files.
# Everything in those directories will be wiped out
# by installation procedure. See svntest-rebuild-generic.sh

SVN_NAME="svn"
SVN_REPO="$TEST_ROOT/$SVN_NAME"

APR_NAME="apr-0.9"
APR_REPO="$TEST_ROOT/$APR_NAME"

APU_NAME="apr-util-0.9"
APU_REPO="$TEST_ROOT/$APU_NAME"

HTTPD_NAME="httpd-2.0"
HTTPD_REPO="$TEST_ROOT/$HTTPD_NAME"


MAKE_OPTS=

# RAMDISK=<yes|no>
RAMDISK=no

# This should correspond with your httpd Listen directive
RA_DAV_CHECK_ARGS="BASE_URL=http://localhost:42024"

#
# Log file name prefix
#
LOG_FILE_PREFIX="$TEST_ROOT/LOG_svntest"

#
# Configure script prefix and object directory names
#
CONFIG_PREFIX="config"
OBJ_STATIC="obj-st"
OBJ_SHARED="obj-sh"

#
# E-mail addresses for reporting
#
FROM="brane@xbc.nu"
TO="svn-breakage@subversion.tigris.org"
ERROR_TO="brane@hermes.si"
REPLY_TO="dev@subversion.tigris.org"

#
# Path to utilities
#
BIN="/bin"
USRBIN="/usr/bin"
LOCALBIN="/usr/local/bin"
OPTBIN="/opt/bin"

# Statically linked svn binary (used for repository updates)
SVN="$TEST_ROOT/static/bin/svn"

# CVS binary (used for updating APR & friends)
CVS="$USRBIN/cvs"

# Path to config.guess (used for generating the mail subject line)
GUESS="/usr/share/libtool/config.guess"

# Path to sendmail
SENDMAIL="/usr/sbin/sendmail"

# Other stuff
BASE64="$USRBIN/base64"
BASE64_E="$BASE64 -e - -"
CAT="$BIN/cat"
CP="$BIN/cp"
CP_F="$CP -f"
CUT="$USRBIN/cut"
GREP="$BIN/grep"
GZIP="$BIN/gzip"
GZIP_C="$GZIP -9c"
ID="$USRBIN/id"
ID_UN="$ID -un"
KILL="$BIN/kill"
MAKE="$USRBIN/make"
MKDIR="$BIN/mkdir"
MKDIR_P="$MKDIR -p"
MOUNT="$BIN/mount"
NICE="$USRBIN/nice"
PS="$BIN/ps"
PS_U="$PS -u"
RM="$BIN/rm"
RM_F="$RM -f"
RM_RF="$RM -rf"
SED="$BIN/sed"
TAIL="$USRBIN/tail"
TAIL_100="$TAIL -n 100"
TOUCH="$USRBIN/touch"
UMOUNT="$BIN/umount"

#
# Helper functions
#

# Start a test
START() {
    TST="$1"
    echo ""
    echo "$2"
}

# Test failed
FAIL() {
    echo "FAIL: $TST" >> $LOG_FILE
    test -n "$1" && eval "$1" "$@"  # Run cleanup code
    umount_ramdisk "$TEST_ROOT/$OBJ_STATIC/subversion/tests"
    umount_ramdisk "$TEST_ROOT/$OBJ_SHARED/subversion/tests"
    exit 1
}

# Test passed
PASS() {
    echo "PASS: $TST" >> $LOG_FILE
}

# Copy a partial log to the main log file
FAIL_LOG() {
    echo >> $LOG_FILE
    echo "Last 100 lines of the log file follow:" >> $LOG_FILE
    $TAIL_100 "$1" >> $LOG_FILE 2>&1
    if [ "x$REV" = "x" ]
    then
        SAVED_LOG="$1.failed"
    else
        SAVED_LOG="$1.$REV.failed"
    fi
    $CP "$1" "$SAVED_LOG" >> $LOG_FILE 2>&1
    echo "Complete log saved in $SAVED_LOG" >> $LOG_FILE
}

# Mount ramdisk conditionally
# check that
# i)  RAMDISK is defined
# ii) Ramdisk isn't already mounted
mount_ramdisk() {
    local mount_dir="$1"
    if test "xyes" == "x$RAMDISK";
    then
        test -z "$mount_dir" && return 1
        
        test -f "$mount_dir/.ramdisk" && {
            echo "Warning: ramdisk exists"
            return 0
        }
    
        $MOUNT "$mount_dir" || return 1
        $TOUCH "$mount_dir/.ramdisk" || return 1
    fi 
    return 0
}

umount_ramdisk() {
    local mount_dir="$1"
    if test "xyes" == "x$RAMDISK";
    then
        test -z "$mount_dir" && return 

        test -f "$mount_dir/.ramdisk" && {
            $UMOUNT "$mount_dir" >> /dev/null 2>&1
        }
    fi
}


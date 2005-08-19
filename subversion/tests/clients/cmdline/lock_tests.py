#!/usr/bin/env python
#
#  lock_tests.py:  testing versioned properties
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2005 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import string, sys, re, os.path, shutil, stat

# Our testing module
import svntest

# A helper function for examining svn:needs-lock
from prop_tests import check_prop

# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem

######################################################################
# Tests

#----------------------------------------------------------------------
# Each test refers to a section in
# notes/locking/locking-functional-spec.txt

# II.A.2, II.C.2.a: Lock a file in wc A as user FOO and make sure we
# have a representation of it.  Checkout wc B as user BAR.  Verify
# that user BAR cannot commit changes to the file nor its properties.
def lock_file(sbox):
  "lock a file and verify that it's locked"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  # lock a file as wc_author
  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_path_b = os.path.join(wc_b, fname)

  svntest.main.file_append(file_path, "This represents a binary file\n")
  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_path)

  # --- Meanwhile, in our other working copy... ---
  err_re = "((.*User jconstant does not own lock on path.*)|(.*423 Locked.*))"

  svntest.main.run_svn(None, 'update', wc_b)
  # -- Try to change a file --
  # change the locked file
  svntest.main.file_append(file_path_b, "Covert tweak\n")

  # attempt (and fail) to commit as user Sally
  svntest.actions.run_and_verify_commit (wc_b, None, None, err_re,
                                         None, None, None, None,
                                         '--username',
                                         svntest.main.wc_author2,
                                         '--password',
                                         svntest.main.wc_passwd,
                                         '-m', '', file_path_b)

  # Revert our change that we failed to commit
  svntest.main.run_svn(None, 'revert', file_path_b)

  # -- Try to change a property --
  # change the locked file's properties
  svntest.main.run_svn(None, 'propset', 'sneakyuser', 'Sally', file_path_b)

  err_re = "((.*User jconstant does not own lock on path.*)" + \
             "|(.*At least one property change failed.*))"

  # attempt (and fail) to commit as user Sally
  svntest.actions.run_and_verify_commit (wc_b, None, None, err_re,
                                         None, None, None, None,
                                         '--username',
                                         svntest.main.wc_author2,
                                         '--password',
                                         svntest.main.wc_passwd,
                                         '-m', '', file_path_b)




#----------------------------------------------------------------------
# II.C.2.b.[12]: Lock a file and commit using the lock.  Make sure the
# lock is released.  Repeat, but request that the lock not be
# released.  Make sure the lock is retained.
def commit_file_keep_lock(sbox):
  "commit a file and keep lock"

  sbox.build()
  wc_dir = sbox.wc_dir

  fname = 'A/mu'
  file_path = os.path.join(sbox.wc_dir, fname)

  # lock fname as wc_author
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'some lock comment', file_path)

  # make a change and commit it, holding lock
  svntest.main.file_append(file_path, "Tweak!\n")
  svntest.main.run_svn(None, 'commit', '-m', '', '--no-unlock', file_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
#  expected_status.tweak(fname, wc_rev=2, writelocked='K')
  expected_status.tweak(fname, wc_rev=2)
  expected_status.tweak(fname, writelocked='K')

  # Make sure the file is still locked
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

def commit_file_unlock(sbox):
  "commit a file and release lock"

  sbox.build()
  wc_dir = sbox.wc_dir

  fname = 'A/mu'
  file_path = os.path.join(sbox.wc_dir, fname)

  # lock fname as wc_author
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'some lock comment', file_path)

  # make a change and commit it, allowing lock to be released
  svntest.main.file_append(file_path, "Tweak!\n")
  svntest.main.run_svn(None, 'commit', '-m', '', file_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.tweak(fname, wc_rev=2)

  # Make sure the file is unlocked
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
def commit_propchange(sbox):
  "commit a locked file with a prop change"

  sbox.build()
  wc_dir = sbox.wc_dir

  fname = 'A/mu'
  file_path = os.path.join(sbox.wc_dir, fname)

  # lock fname as wc_author
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'some lock comment', file_path)

  # make a property change and commit it, allowing lock to be released
  svntest.main.run_svn(None, 'propset', 'blue', 'azul', file_path)
  svntest.main.run_svn(None, 'commit', '-m', '', file_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.tweak(fname, wc_rev=2)

  # Make sure the file is unlocked
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# II.C.2.c: Lock a file in wc A as user FOO.  Attempt to unlock same
# file in same wc as user BAR.  Should fail.
#
# Attempt again with --force.  Should succeed.
#
# II.C.2.c: Lock a file in wc A as user FOO.  Attempt to unlock same
# file in wc B as user FOO.  Should fail.
#
# Attempt again with --force.  Should succeed.
def break_lock(sbox):
  "lock a file and verify lock breaking behavior"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  # lock a file as wc_author
  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_path_b = os.path.join(wc_b, fname)

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_path)

  # --- Meanwhile, in our other working copy... ---

  svntest.main.run_svn(None, 'update', wc_b)

  # attempt (and fail) to unlock file

  # This should give a "iota' is not locked in this working copy" error
  svntest.actions.run_and_verify_svn(None, None, svntest.SVNAnyOutput,
                                     'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     file_path_b)

  svntest.actions.run_and_verify_svn(None, None, None,
                                     'unlock', '--force',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     file_path_b)

#----------------------------------------------------------------------
# II.C.2.d: Lock a file in wc A as user FOO.  Attempt to lock same
# file in wc B as user BAR.  Should fail.
#
# Attempt again with --force.  Should succeed.
#
# II.C.2.d: Lock a file in wc A as user FOO.  Attempt to lock same
# file in wc B as user FOO.  Should fail.
#
# Attempt again with --force.  Should succeed.
def steal_lock(sbox):
  "lock a file and verify lock stealing behavior"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  # lock a file as wc_author
  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_path_b = os.path.join(wc_b, fname)

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_path)

  # --- Meanwhile, in our other working copy... ---

  svntest.main.run_svn(None, 'update', wc_b)

  # attempt (and fail) to lock file

  # This should give a "iota' is not locked in this working copy" error
  svntest.actions.run_and_verify_svn(None, None, svntest.SVNAnyOutput,
                                     'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'trying to break', file_path_b)

  svntest.actions.run_and_verify_svn(None, None, None,
                                     'lock', '--force',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'trying to break', file_path_b)

#----------------------------------------------------------------------
# II.B.2, II.C.2.e: Lock a file in wc A.  Query wc for the
# lock and verify that all lock fields are present and correct.
def examine_lock(sbox):
  "examine the fields of a lockfile for correctness"

  sbox.build()
  wc_dir = sbox.wc_dir

  fname = 'iota'
  comment = 'This is a lock test.'
  file_path = os.path.join(sbox.wc_dir, fname)

  # lock a file as wc_author
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', comment, file_path)

  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'info', file_path)

  lock_info = output[-6:-1]
  if ((len(lock_info) != 5)
      or (lock_info[0][0:28] != 'Lock Token: opaquelocktoken:')
      or (lock_info[1] != 'Lock Owner: ' + svntest.main.wc_author + '\n')
      or (lock_info[2][0:13] != 'Lock Created:')
      or (lock_info[4] != comment + '\n')):
    raise svntest.Failure



#----------------------------------------------------------------------
# II.C.1: Lock a file in wc A.  Check out wc B.  Break the lock in wc
# B.  Verify that wc A gracefully cleans up the lock via update as
# well as via commit.
def handle_defunct_lock(sbox):
  "verify behavior when a lock in a wc is defunct"

  sbox.build()
  wc_dir = sbox.wc_dir


  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)

  # set up our expected status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)

  # lock the file
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_path)

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)
  file_path_b = os.path.join(wc_b, fname)

  # --- Meanwhile, in our other working copy... ---

  # Try unlocking the file in the second wc.
  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     file_path_b)


  # update the 1st wc, which should clear the lock there
  svntest.main.run_svn(None, 'update', wc_dir)

  # Make sure the file is unlocked
  svntest.actions.run_and_verify_status(wc_dir, expected_status)



#----------------------------------------------------------------------
# II.B.1: Set "svn:needs-lock" property on file in wc A.  Checkout wc
# B and verify that that file is set as read-only.
#
# Tests propset, propdel, lock, and unlock
def enforce_lock(sbox):
  "verify svn:needs-lock read-only behavior"

  sbox.build()
  wc_dir = sbox.wc_dir

  iota_path = os.path.join(wc_dir, 'iota')
  lambda_path = os.path.join(wc_dir, 'A', 'B', 'lambda')
  mu_path = os.path.join(wc_dir, 'A', 'mu')

  # svn:needs-lock value should be forced to a '*'
  svntest.main.run_svn(None, 'propset', 'svn:needs-lock', 'foo', iota_path)
  svntest.main.run_svn(None, 'propset', 'svn:needs-lock', '', lambda_path)
  svntest.main.run_svn(None, 'propset', 'svn:needs-lock', '      ', mu_path)

  # Check svn:needs-lock
  check_prop('svn:needs-lock', iota_path, ['*'])
  check_prop('svn:needs-lock', lambda_path, ['*'])
  check_prop('svn:needs-lock', mu_path, ['*'])

  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', iota_path, lambda_path, mu_path)

  # Now make sure that the perms were flipped on all files
  if os.name == 'posix':
    mode = stat.S_IWGRP | stat.S_IWOTH | stat.S_IWRITE
    if ((os.stat (iota_path)[0] & mode)
        or (os.stat (lambda_path)[0] & mode)
        or (os.stat (mu_path)[0] & mode)):
      print "Setting 'svn:needs-lock' property on a file failed to set"
      print "file mode to read-only."
      raise svntest.Failure

    # obtain a lock on one of these files...
    svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       '-m', '', iota_path)

    # ...and verify that the write bit gets set...
    if not (os.stat (iota_path)[0] & mode):
      print "Locking a file with 'svn:needs-lock' failed to set write bit."
      raise svntest.Failure

    # ...and unlock it...
    svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       iota_path)

    # ...and verify that the write bit gets unset
    if (os.stat (iota_path)[0] & mode):
      print "Unlocking a file with 'svn:needs-lock' failed to unset write bit."
      raise svntest.Failure

    # Verify that removing the property restores the file to read-write
    svntest.main.run_svn(None, 'propdel', 'svn:needs-lock', iota_path)
    if not (os.stat (iota_path)[0] & mode):
      print "Deleting 'svn:needs-lock' failed to set write bit."
      raise svntest.Failure

#----------------------------------------------------------------------
# Test that updating a file with the "svn:needs-lock" property works,
# especially on Windows, where renaming A to B fails if B already
# exists and has its read-only bit set.  See also issue #2278.
def update_while_needing_lock(sbox):
  "update handles svn:needs-lock correctly"

  sbox.build()
  wc_dir = sbox.wc_dir

  iota_path = os.path.join(wc_dir, 'iota')
  svntest.main.run_svn(None, 'propset', 'svn:needs-lock', '*', iota_path)
  svntest.main.run_svn(None, 'commit', '-m', 'log msg', iota_path)
  svntest.main.run_svn(None, 'up', wc_dir)

  # Lock, modify, commit, unlock, to create r3.
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', iota_path)
  svntest.main.file_append(iota_path, "This line added in r2.\n")
  svntest.main.run_svn(None, 'commit', '-m', '', iota_path) # auto-unlocks

  # Backdate to r2.
  svntest.main.run_svn(None, 'update', '-r2', iota_path)

  # Try updating forward to r3 again.  This is where the bug happened.
  svntest.main.run_svn(None, 'update', '-r3', iota_path)


#----------------------------------------------------------------------
# Tests update / checkout with changing props
def defunct_lock(sbox):
  "verify svn:needs-lock behavior with defunct lock"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  iota_path = os.path.join(wc_dir, 'iota')
  iota_path_b = os.path.join(wc_b, 'iota')

  mode = stat.S_IWGRP | stat.S_IWOTH | stat.S_IWRITE

# Set the prop in wc a
  svntest.main.run_svn(None, 'propset', 'svn:needs-lock', 'foo', iota_path)

  # commit r2
  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', iota_path)

  # update wc_b
  svntest.main.run_svn(None, 'update', wc_b)

  # lock iota in wc_b
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', iota_path_b)


  # break the lock iota in wc a
  svntest.actions.run_and_verify_svn(None, None, None, 'lock', '--force',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', iota_path)
  # update wc_b
  svntest.main.run_svn(None, 'update', wc_b)

  # make sure that iota got set to read-only
  if (os.stat (iota_path_b)[0] & mode):
    print "Upon removal of a defunct lock, a file with 'svn:needs-lock'"
    print "was not set back to read-only"
    raise svntest.Failure



#----------------------------------------------------------------------
# Tests dealing with a lock on a deleted path 
def deleted_path_lock(sbox):
  "verify lock removal on a deleted path"

  sbox.build()
  wc_dir = sbox.wc_dir

  iota_path = os.path.join(wc_dir, 'iota')
  iota_url = svntest.main.current_repo_url + '/iota'

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', iota_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'delete', iota_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-unlock',
                                     '-m', '', iota_path)

  # Now make sure that we can delete the lock from iota via a URL
  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     iota_url)



#----------------------------------------------------------------------
# Tests dealing with locking and unlocking
def lock_unlock(sbox):
  "lock and unlock some files"

  sbox.build()
  wc_dir = sbox.wc_dir

  pi_path = os.path.join(wc_dir, 'A', 'D', 'G', 'pi')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  tau_path = os.path.join(wc_dir, 'A', 'D', 'G', 'tau')

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', pi_path, rho_path, tau_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     pi_path, rho_path, tau_path)


#----------------------------------------------------------------------
# Tests dealing with directory deletion and locks
def deleted_dir_lock(sbox):
  "verify removal of a directory with locks inside"

  sbox.build()
  wc_dir = sbox.wc_dir

  parent_dir = os.path.join(wc_dir, 'A', 'D', 'G')
  pi_path = os.path.join(wc_dir, 'A', 'D', 'G', 'pi')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  tau_path = os.path.join(wc_dir, 'A', 'D', 'G', 'tau')

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', pi_path, rho_path, tau_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'delete', parent_dir)

  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-unlock',
                                     '-m', '', parent_dir)

#----------------------------------------------------------------------
# III.c : Lock a file and check the output of 'svn stat' from the same
# working copy and another.
def lock_status(sbox):
  "verify status of lock in working copy"
  sbox.build()
  wc_dir = sbox.wc_dir

   # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  # lock a file as wc_author
  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)

  svntest.main.file_append(file_path, "This is a spreadsheet\n")
  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)

  svntest.main.run_svn(None, 'lock', 
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path) 

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)  
  expected_status.tweak(wc_rev=1)
  expected_status.tweak(fname, wc_rev=2)
  expected_status.tweak(fname, writelocked='K')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Verify status again after modifying the file
  svntest.main.file_append(file_path, "check stat output after mod")

  expected_status.tweak(fname, status='M ')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Verify status of lock from another working copy
  svntest.main.run_svn(None, 'update', wc_b)
  expected_status = svntest.actions.get_virginal_state(wc_b, 2)
  expected_status.tweak(fname, writelocked='O')

  svntest.actions.run_and_verify_status(wc_b, expected_status)

#----------------------------------------------------------------------
# III.c : Steal lock on a file from another working copy with 'svn lock
# --force', and check the status of lock in the repository from the 
# working copy in which the file was initially locked.
def stolen_lock_status (sbox):
  "verify status of stolen lock"
  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  # lock a file as wc_author
  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_path_b = os.path.join(wc_b, fname)

  svntest.main.file_append(file_path, "This is a spreadsheet\n")
  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)

  svntest.main.run_svn(None, 'lock',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.tweak(fname, wc_rev=2)
  expected_status.tweak(fname, writelocked='K')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Forcibly lock same file (steal lock) from another working copy
  svntest.main.run_svn(None, 'update', wc_b)
  svntest.main.run_svn(None, 'lock',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', '--force', file_path_b)
 
  # Verify status from working copy where file was initially locked
  expected_status.tweak(fname, writelocked='T')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# III.c : Break lock from another working copy with 'svn unlock --force'
# and verify the status of the lock in the repository with 'svn stat -u'
# from the working copy in the file was initially locked
def broken_lock_status (sbox):
  "verify status of broken lock"
  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  # lock a file as wc_author
  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_path_b = os.path.join(wc_b, fname)

  svntest.main.file_append(file_path, "This is a spreadsheet\n")
  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)
  svntest.main.run_svn(None, 'lock',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.tweak(fname, wc_rev=2)
  expected_status.tweak(fname, writelocked='K')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Forcibly unlock the same file (break lock) from another working copy
  svntest.main.run_svn(None, 'update', wc_b)
  svntest.main.run_svn(None, 'unlock',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '--force', file_path_b)

  # Verify status from working copy where file was initially locked
  expected_status.tweak(fname, writelocked='B')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------


#----------------------------------------------------------------------
# Check that locking an out-of-date file fails.
def out_of_date(sbox):
  "lock an out-of-date file and ensure failure"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a second copy of the working copy
  wc_b = sbox.add_wc_path('_b')
  svntest.actions.duplicate_dir(wc_dir, wc_b)

  fname = 'iota'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_path_b = os.path.join(wc_b, fname)

  # Make a new revision of the file in the first WC.
  svntest.main.file_append(file_path, "This represents a binary file\n")
  svntest.main.run_svn(None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', file_path)

  # --- Meanwhile, in our other working copy... ---
  svntest.actions.run_and_verify_svn(None, None, svntest.SVNAnyOutput, 'lock',
                                     '--username', svntest.main.wc_author2,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_path_b)

#----------------------------------------------------------------------
# Tests reverting a svn:needs-lock file
def revert_lock(sbox):
  "verify svn:needs-lock behavior with revert"

  sbox.build()
  wc_dir = sbox.wc_dir

  iota_path = os.path.join(wc_dir, 'iota')

  mode = stat.S_IWGRP | stat.S_IWOTH | stat.S_IWRITE

  # set the prop in wc 
  svntest.actions.run_and_verify_svn(None, None, None, 'propset',
                                  'svn:needs-lock', 'foo', iota_path)

  # commit r2
  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', iota_path)

  # make sure that iota got set to read-only
  if (os.stat (iota_path)[0] & mode):
    print "Committing a file with 'svn:needs-lock'"
    print "did not set the file to read-only"
    raise svntest.Failure

  # verify status is as we expect
  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.tweak('iota', wc_rev=2)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # remove read-only-ness
  svntest.actions.run_and_verify_svn(None, None, None, 'propdel',
                                  'svn:needs-lock', iota_path)

  # make sure that iota got read-only-ness removed
  if (os.stat (iota_path)[0] & mode == 0):
    print "Deleting the 'svn:needs-lock' property "
    print "did not remove read-only-ness"
    raise svntest.Failure
  
  # revert the change
  svntest.actions.run_and_verify_svn(None, None, None, 'revert', iota_path)

  # make sure that iota got set back to read-only
  if (os.stat (iota_path)[0] & mode):
    print "Reverting a file with 'svn:needs-lock'"
    print "did not set the file back to read-only"
    raise svntest.Failure
 
  # try propdel and revert from a different directory so
  # full filenames are used
  extra_name = 'xx'

  # now lock the file
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       '-m', '', iota_path)
  
  # modify it
  svntest.main.file_append(iota_path, "This line added\n")

  expected_status.tweak(wc_rev=1)
  expected_status.tweak('iota', wc_rev=2)
  expected_status.tweak('iota', status='M ', writelocked='K')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  
  # revert it
  svntest.actions.run_and_verify_svn(None, None, None, 'revert', iota_path)

  # make sure it is still writable since we have the lock
  if (os.stat (iota_path)[0] & mode == 0):
    print "Reverting a 'svn:needs-lock' file (with lock in wc) "
    print "did not leave the file writable"
    raise svntest.Failure
  

#----------------------------------------------------------------------
def examine_lock_via_url(sbox):
  "examine the fields of a lock from a URL"

  sbox.build()
  wc_dir = sbox.wc_dir

  fname = 'iota'
  comment = 'This is a lock test.'
  file_path = os.path.join(sbox.wc_dir, fname)
  file_url = svntest.main.current_repo_url + '/' + fname

  # lock a file as wc_author
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author2,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-auth-cache',
                                     '-m', comment, file_path)

  output, err = svntest.actions.run_and_verify_svn(None, None, None, 'info',
                                                   file_url)

  match_line = 'Lock Owner: ' + svntest.main.wc_author2 + '\n'

  if not match_line in output:
    print "Error: expected output '%s' not found in output." % match_line
    raise svntest.Failure

#----------------------------------------------------------------------
def lock_several_files(sbox):
  "lock/unlock several files in one go"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Deliberately have no direct child of A as a target
  iota_path = os.path.join(sbox.wc_dir, 'iota')
  lambda_path = os.path.join(sbox.wc_dir, 'A', 'B', 'lambda')
  alpha_path = os.path.join(sbox.wc_dir, 'A', 'B', 'E', 'alpha')

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author2,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-auth-cache',
                                     '-m', 'lock several',
                                     iota_path, lambda_path, alpha_path)
  
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('iota', 'A/B/lambda', 'A/B/E/alpha', writelocked='K')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author2,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-auth-cache',
                                     iota_path, lambda_path, alpha_path)

  expected_status.tweak('iota', 'A/B/lambda', 'A/B/E/alpha', writelocked=None)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# A regression test for a bug when svn:needs-lock and svn:executable
# interact badly. The bug was fixed in trunk @ r14859.
def lock_and_exebit1(sbox):
  "svn:needs-lock and svn:executable, part I"

  mode_w = stat.S_IWUSR
  mode_x = stat.S_IXUSR
  mode_r = stat.S_IRUSR
  
  sbox.build()
  wc_dir = sbox.wc_dir

  gamma_path = os.path.join(wc_dir, 'A', 'D', 'gamma')
  
  svntest.actions.run_and_verify_svn(None, None, None, 'ps',
                                     'svn:needs-lock', ' ', gamma_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'ps',
                                     'svn:executable', ' ', gamma_path)
  
  # commit
  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', gamma_path)
  # mode should be +r, -w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r
      or gamma_stat & mode_w
      or not gamma_stat & mode_x):
    print "Committing a file with 'svn:needs-lock, svn:executable'"
    print "did not set the file to read-only, executable"
    raise svntest.Failure

  # lock
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', gamma_path)
  # mode should be +r, +w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r 
      or not gamma_stat & mode_w 
      or not gamma_stat & mode_x):
    print "Locking a file with 'svn:needs-lock, svn:executable'"
    print "did not set the file to read-write, executable"
    raise svntest.Failure

  # modify
  svntest.main.file_append(gamma_path, "check stat output after mod & unlock")
  
  # unlock
  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     gamma_path)
  
  # Mode should be +r, -w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r 
      or gamma_stat & mode_w 
      or not gamma_stat & mode_x):
    print "Unlocking a file with 'svn:needs-lock, svn:executable'"
    print "did not set the file to read-only, executable"
    raise svntest.Failure
  
  # ci
  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', gamma_path)
  
  # Mode should be still +r, -w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r 
      or gamma_stat & mode_w 
      or not gamma_stat & mode_x):
    print "Commiting a file with 'svn:needs-lock, svn:executable'"
    print "after unlocking modified file's permissions"
    raise svntest.Failure


#----------------------------------------------------------------------
# A variant of lock_and_exebit1: same test without unlock
def lock_and_exebit2(sbox):
  "svn:needs-lock and svn:executable, part II"

  mode_w = stat.S_IWUSR
  mode_x = stat.S_IXUSR
  mode_r = stat.S_IRUSR
  
  sbox.build()
  wc_dir = sbox.wc_dir

  gamma_path = os.path.join(wc_dir, 'A', 'D', 'gamma')
  
  svntest.actions.run_and_verify_svn(None, None, None, 'ps',
                                     'svn:needs-lock', ' ', gamma_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'ps',
                                     'svn:executable', ' ', gamma_path)
  
  # commit
  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', gamma_path)
  # mode should be +r, -w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r
      or gamma_stat & mode_w
      or not gamma_stat & mode_x):
    print "Committing a file with 'svn:needs-lock, svn:executable'"
    print "did not set the file to read-only, executable"
    raise svntest.Failure

  # lock
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', gamma_path)
  # mode should be +r, +w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r 
      or not gamma_stat & mode_w 
      or not gamma_stat & mode_x):
    print "Locking a file with 'svn:needs-lock, svn:executable'"
    print "did not set the file to read-write, executable"
    raise svntest.Failure

  # modify
  svntest.main.file_append(gamma_path, "check stat output after mod & unlock")
  
  # commit
  svntest.actions.run_and_verify_svn(None, None, None, 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', gamma_path)
  
  # Mode should be +r, -w, +x
  gamma_stat = os.stat (gamma_path)[0]
  if (not gamma_stat & mode_r 
      or gamma_stat & mode_w 
      or not gamma_stat & mode_x):
    print "Commiting a file with 'svn:needs-lock, svn:executable'"
    print "did not set the file to read-only, executable"
    raise svntest.Failure


#----------------------------------------------------------------------
def lock_switched_files(sbox):
  "lock/unlock switched files"

  sbox.build()
  wc_dir = sbox.wc_dir

  gamma_path = os.path.join(wc_dir, 'A', 'D', 'gamma')
  lambda_path = os.path.join(wc_dir, 'A', 'B', 'lambda')
  iota_URL = svntest.main.current_repo_url + '/iota'
  alpha_URL = svntest.main.current_repo_url + '/A/B/E/alpha'

  svntest.actions.run_and_verify_svn(None, None, None, 'switch',
                                     iota_URL, gamma_path)
  svntest.actions.run_and_verify_svn(None, None, None, 'switch',
                                     alpha_URL, lambda_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/D/gamma', 'A/B/lambda', switched='S')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-auth-cache',
                                     '-m', 'lock several',
                                     gamma_path, lambda_path)

  expected_status.tweak('A/D/gamma', 'A/B/lambda', writelocked='K')
  expected_status.tweak('A/B/E/alpha', 'iota', writelocked='O')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '--no-auth-cache',
                                     gamma_path, lambda_path)

  expected_status.tweak('A/D/gamma', 'A/B/lambda', writelocked=None)
  expected_status.tweak('A/B/E/alpha', 'iota', writelocked=None)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

def lock_uri_encoded(sbox):
  "lock and unlock a file with an URI-unsafe name"

  sbox.build()
  wc_dir = sbox.wc_dir

  # lock a file as wc_author
  fname = 'amazing space'
  file_path = os.path.join(wc_dir, fname)

  svntest.main.file_append(file_path, "This represents a binary file\n")
  svntest.actions.run_and_verify_svn(None, None, None, "add", file_path)

  expected_output = svntest.wc.State(wc_dir, {
    fname : Item(verb='Adding'),
    })

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({ fname: Item(wc_rev=2, status='  ') })

  # Commit the file.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        file_path)

  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_path)

  # Make sure that the file was locked.
  expected_status.tweak(fname, writelocked='K')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     file_path)

  # Make sure it was successfully unlocked again.
  expected_status.tweak(fname, writelocked=None)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # And now the URL case.
  file_url = svntest.main.current_repo_url + '/' + fname
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', '', file_url)

  # Make sure that the file was locked.
  expected_status.tweak(fname, writelocked='O')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  svntest.actions.run_and_verify_svn(None, None, None, 'unlock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     file_url)

  # Make sure it was successfully unlocked again.
  expected_status.tweak(fname, writelocked=None)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)


def commit_xml_unsafe_file_unlock(sbox):
  "commit file with xml-unsafe name and release lock"

  sbox.build()
  wc_dir = sbox.wc_dir

  fname = 'foo & bar'
  file_path = os.path.join(sbox.wc_dir, fname)
  svntest.main.file_append(file_path, "Initial data.\n")
  svntest.main.run_svn(None, 'add', file_path)
  svntest.main.run_svn(None, 'commit', '-m', '', file_path)

  # lock fname as wc_author
  svntest.actions.run_and_verify_svn(None, None, None, 'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'some lock comment', file_path)

  # make a change and commit it, allowing lock to be released
  svntest.main.file_append(file_path, "Followup data.\n")
  svntest.main.run_svn(None, 'commit', '-m', '', file_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 3)
  expected_status.tweak(wc_rev=1)
  expected_status.add({ fname : Item(status='  ', wc_rev=3), })

  # Make sure the file is unlocked
  svntest.actions.run_and_verify_status(wc_dir, expected_status)


########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              lock_file,
              commit_file_keep_lock,
              commit_file_unlock,
              commit_propchange,
              break_lock,
              steal_lock,
              examine_lock,
              handle_defunct_lock,
              enforce_lock,
              defunct_lock,
              deleted_path_lock,
              lock_unlock,
              deleted_dir_lock,
              lock_status,
              stolen_lock_status,
              broken_lock_status,
              out_of_date,
              update_while_needing_lock,
              revert_lock,
              examine_lock_via_url,
              lock_several_files,
              Skip(lock_and_exebit1, (os.name != 'posix')),
              Skip(lock_and_exebit2, (os.name != 'posix')),
              lock_switched_files,
              lock_uri_encoded,
              commit_xml_unsafe_file_unlock,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

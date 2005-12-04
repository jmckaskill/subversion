#!/usr/bin/env python
#
#  schedule_tests.py:  testing working copy scheduling
#                      (adds, deletes, reversion)
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2004 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import string, sys, os, shutil

# Our testing module
import svntest
from svntest import SVNAnyOutput

# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem


######################################################################
# Tests
#
#   Each test must return on success or raise on failure.
#
#   NOTE: Tests in this section should be written in triplets.  First
#   compose a test which make schedule changes and local mods, and
#   verifies that status output is as expected.  Secondly, compose a
#   test which calls the first test (to do all the dirty work), then
#   test reversion of those changes.  Finally, compose a third test
#   which, again, calls the first test to "set the stage", and then
#   commit those changes.
#
#----------------------------------------------------------------------

#######################################################################
#  Stage I - Schedules and modifications, verified with `svn status'
#

def add_files(sbox):
  "schedule: add some files"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create some files, then schedule them for addition
  delta_path = os.path.join(wc_dir, 'delta')
  zeta_path = os.path.join(wc_dir, 'A', 'B', 'zeta')
  epsilon_path = os.path.join(wc_dir, 'A', 'D', 'G', 'epsilon')
  
  svntest.main.file_append(delta_path, "This is the file 'delta'.")
  svntest.main.file_append(zeta_path, "This is the file 'zeta'.")
  svntest.main.file_append(epsilon_path, "This is the file 'epsilon'.")
  
  svntest.main.run_svn(None, 'add', delta_path, zeta_path, epsilon_path)
  
  # Make sure the adds show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'delta' : Item(status='A ', wc_rev=0),
    'A/B/zeta' : Item(status='A ', wc_rev=0),
    'A/D/G/epsilon' : Item(status='A ', wc_rev=0),
    })

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------

def add_directories(sbox):
  "schedule: add some directories"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create some directories, then schedule them for addition
  X_path = os.path.join(wc_dir, 'X')
  Y_path = os.path.join(wc_dir, 'A', 'C', 'Y')
  Z_path = os.path.join(wc_dir, 'A', 'D', 'H', 'Z')
  
  os.mkdir(X_path)
  os.mkdir(Y_path)
  os.mkdir(Z_path)
  
  svntest.main.run_svn(None, 'add', X_path, Y_path, Z_path)
  
  # Make sure the adds show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'X' : Item(status='A ', wc_rev=0),
    'A/C/Y' : Item(status='A ', wc_rev=0),
    'A/D/H/Z' : Item(status='A ', wc_rev=0),
    })

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------

def nested_adds(sbox):
  "schedule: add some nested files and directories"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create some directories then schedule them for addition
  X_path = os.path.join(wc_dir, 'X')
  Y_path = os.path.join(wc_dir, 'A', 'C', 'Y')
  Z_path = os.path.join(wc_dir, 'A', 'D', 'H', 'Z')

  os.mkdir(X_path)
  os.mkdir(Y_path)
  os.mkdir(Z_path)

  # Now, create some files and directories to put into our newly added
  # directories
  P_path = os.path.join(X_path, 'P')
  Q_path = os.path.join(Y_path, 'Q')
  R_path = os.path.join(Z_path, 'R')

  os.mkdir(P_path)
  os.mkdir(Q_path)
  os.mkdir(R_path)
  
  delta_path = os.path.join(X_path, 'delta')
  epsilon_path = os.path.join(Y_path, 'epsilon')
  upsilon_path = os.path.join(Y_path, 'upsilon')
  zeta_path = os.path.join(Z_path, 'zeta')

  svntest.main.file_append(delta_path, "This is the file 'delta'.")
  svntest.main.file_append(epsilon_path, "This is the file 'epsilon'.")
  svntest.main.file_append(upsilon_path, "This is the file 'upsilon'.")
  svntest.main.file_append(zeta_path, "This is the file 'zeta'.")

  # Finally, let's try adding our new files and directories
  svntest.main.run_svn(None, 'add', X_path, Y_path, Z_path)
    
  # Make sure the adds show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'X' : Item(status='A ', wc_rev=0),
    'A/C/Y' : Item(status='A ', wc_rev=0),
    'A/D/H/Z' : Item(status='A ', wc_rev=0),
    'X/P' : Item(status='A ', wc_rev=0),
    'A/C/Y/Q' : Item(status='A ', wc_rev=0),
    'A/D/H/Z/R' : Item(status='A ', wc_rev=0),
    'X/delta' : Item(status='A ', wc_rev=0),
    'A/C/Y/epsilon' : Item(status='A ', wc_rev=0),
    'A/C/Y/upsilon' : Item(status='A ', wc_rev=0),
    'A/D/H/Z/zeta' : Item(status='A ', wc_rev=0),
    })

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------

def add_executable(sbox):
  "schedule: add some executable files"

  sbox.build()

  def runTest(wc_dir, fileName, perm, executable):
    fileName = os.path.join(wc_dir, fileName)
    if executable:
      expected_out = ["*\n"]
    else:
      expected_out = []
    f = open(fileName,"w")
    f.close()
    os.chmod(fileName,perm)
    svntest.main.run_svn(None, 'add', fileName)
    svntest.actions.run_and_verify_svn(None, expected_out, [],
                                       'propget', "svn:executable", fileName)
    
  test_cases = [
    ("all_exe",   0777, 1),
    ("none_exe",  0666, 0),
    ("user_exe",  0766, 1),
    ("group_exe", 0676, 0),
    ("other_exe", 0667, 0),
    ]
  for test_case in test_cases:
    runTest(sbox.wc_dir, *test_case)

#----------------------------------------------------------------------

def delete_files(sbox):
  "schedule: delete some files"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Schedule some files for deletion
  iota_path = os.path.join(wc_dir, 'iota')
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  omega_path = os.path.join(wc_dir, 'A', 'D', 'H', 'omega')
  
  svntest.main.run_svn(None, 'del', iota_path, mu_path, rho_path, omega_path)
    
  # Make sure the deletes show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('iota', 'A/mu', 'A/D/G/rho', 'A/D/H/omega',
                        status='D ')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------

def delete_dirs(sbox):
  "schedule: delete some directories"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Schedule some directories for deletion (this is recursive!)
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  H_path = os.path.join(wc_dir, 'A', 'D', 'H')
  alpha_path = os.path.join(E_path, 'alpha')
  beta_path  = os.path.join(E_path, 'beta')
  chi_path   = os.path.join(H_path, 'chi')
  omega_path = os.path.join(H_path, 'omega')
  psi_path   = os.path.join(H_path, 'psi')
  
  # Now, delete (recursively) the directories.
  svntest.main.run_svn(None, 'del', E_path, F_path, H_path)
    
  # Make sure the deletes show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/E', 'A/B/E/alpha', 'A/B/E/beta',
                        'A/B/F',
                        'A/D/H', 'A/D/H/chi', 'A/D/H/omega', 'A/D/H/psi',
                        status='D ')

  svntest.actions.run_and_verify_status(wc_dir, expected_status)


#######################################################################
#  Stage II - Reversion of changes made in Stage I
#

def check_reversion(files, output):
  expected_output = []
  for file in files:
    expected_output = expected_output + ["Reverted '" + file + "'\n"]
  output.sort()
  expected_output.sort()
  if output != expected_output:
    print "Expected output:", expected_output
    print "Actual output:  ", output
    raise svntest.Failure
    
#----------------------------------------------------------------------

def revert_add_files(sbox):
  "revert: add some files"

  add_files(sbox)
  wc_dir = sbox.wc_dir

  # Revert our changes recursively from wc_dir.
  delta_path = os.path.join(wc_dir, 'delta')
  zeta_path = os.path.join(wc_dir, 'A', 'B', 'zeta')
  epsilon_path = os.path.join(wc_dir, 'A', 'D', 'G', 'epsilon')
  files = [delta_path, zeta_path, epsilon_path]

  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'revert',
                                                   '--recursive', wc_dir)
  check_reversion(files, output)

#----------------------------------------------------------------------

def revert_add_directories(sbox):
  "revert: add some directories"

  add_directories(sbox)
  wc_dir = sbox.wc_dir

  # Revert our changes recursively from wc_dir.
  X_path = os.path.join(wc_dir, 'X')
  Y_path = os.path.join(wc_dir, 'A', 'C', 'Y')
  Z_path = os.path.join(wc_dir, 'A', 'D', 'H', 'Z')
  files = [X_path, Y_path, Z_path]
  
  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'revert',
                                                   '--recursive', wc_dir)
  check_reversion(files, output)

#----------------------------------------------------------------------

def revert_nested_adds(sbox):
  "revert: add some nested files and directories"

  nested_adds(sbox)
  wc_dir = sbox.wc_dir

  # Revert our changes recursively from wc_dir.
  X_path = os.path.join(wc_dir, 'X')
  Y_path = os.path.join(wc_dir, 'A', 'C', 'Y')
  Z_path = os.path.join(wc_dir, 'A', 'D', 'H', 'Z')
  files = [X_path, Y_path, Z_path]

  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'revert',
                                                   '--recursive', wc_dir)
  check_reversion(files, output)
  
#----------------------------------------------------------------------

def revert_add_executable(sbox):
  "revert: add some executable files"

  add_executable(sbox)
  wc_dir = sbox.wc_dir
  
  all_path = os.path.join(wc_dir, 'all_exe')
  none_path = os.path.join(wc_dir, 'none_exe')
  user_path = os.path.join(wc_dir, 'user_exe')
  group_path = os.path.join(wc_dir, 'group_exe')
  other_path = os.path.join(wc_dir, 'other_exe')
  files = [all_path, none_path, user_path, group_path, other_path]

  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'revert',
                                                   '--recursive', wc_dir)
  check_reversion(files, output)

#----------------------------------------------------------------------

def revert_delete_files(sbox):
  "revert: delete some files"

  delete_files(sbox)
  wc_dir = sbox.wc_dir

  # Revert our changes recursively from wc_dir.
  iota_path = os.path.join(wc_dir, 'iota')
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  omega_path = os.path.join(wc_dir, 'A', 'D', 'H', 'omega')
  files = [iota_path, mu_path, omega_path, rho_path]
  
  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'revert',
                                                   '--recursive', wc_dir)
  check_reversion(files, output)

#----------------------------------------------------------------------

def revert_delete_dirs(sbox):
  "revert: delete some directories"

  delete_dirs(sbox)
  wc_dir = sbox.wc_dir

  # Revert our changes recursively from wc_dir.
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  H_path = os.path.join(wc_dir, 'A', 'D', 'H')
  alpha_path = os.path.join(E_path, 'alpha')
  beta_path  = os.path.join(E_path, 'beta')
  chi_path   = os.path.join(H_path, 'chi')
  omega_path = os.path.join(H_path, 'omega')
  psi_path   = os.path.join(H_path, 'psi')
  files = [E_path, F_path, H_path,
           alpha_path, beta_path, chi_path, omega_path, psi_path]

  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'revert',
                                                   '--recursive', wc_dir)
  check_reversion(files, output)

#######################################################################
#  Stage III - Commit of modifications made in Stage 1
#

def commit_add_files(sbox):
  "commit: add some files"

  add_files(sbox)

  raise svntest.Failure

#----------------------------------------------------------------------

def commit_add_directories(sbox):
  "commit: add some directories"

  add_directories(sbox)

  raise svntest.Failure

#----------------------------------------------------------------------

def commit_nested_adds(sbox):
  "commit: add some nested files and directories"

  nested_adds(sbox)

  raise svntest.Failure

#----------------------------------------------------------------------

def commit_add_executable(sbox):
  "commit: add some executable files"

  add_executable(sbox)

  raise svntest.Failure


#----------------------------------------------------------------------

def commit_delete_files(sbox):
  "commit: delete some files"

  delete_files(sbox)

  raise svntest.Failure

#----------------------------------------------------------------------

def commit_delete_dirs(sbox):
  "commit: delete some directories"

  delete_dirs(sbox)

  raise svntest.Failure

#----------------------------------------------------------------------
# Regression test for issue #863:
#
# Suppose here is a scheduled-add file or directory which is
# also missing.  If I want to make the working copy forget all
# knowledge of the item ("unschedule" the addition), then either 'svn
# revert' or 'svn rm' will make that happen, with no errors.  The
# entry is simply removed from the entries file.

def unschedule_missing_added(sbox):
  "unschedule addition on missing items"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create some files and dirs, then schedule them for addition
  file1_path = os.path.join(wc_dir, 'file1')
  file2_path = os.path.join(wc_dir, 'file2')
  dir1_path = os.path.join(wc_dir, 'dir1')
  dir2_path = os.path.join(wc_dir, 'dir2')
  
  svntest.main.file_append(file1_path, "This is the file 'file1'.")
  svntest.main.file_append(file2_path, "This is the file 'file2'.")
  svntest.main.run_svn(None, 'add', file1_path, file2_path)
  svntest.main.run_svn(None, 'mkdir', dir1_path, dir2_path)
  
  # Make sure the 4 adds show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'file1' : Item(status='A ', wc_rev=0),
    'file2' : Item(status='A ', wc_rev=0),
    'dir1' : Item(status='A ', wc_rev=0),
    'dir2' : Item(status='A ', wc_rev=0),
    })

  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Poof, all 4 added things are now missing in action.
  os.remove(file1_path)
  os.remove(file2_path)
  svntest.main.safe_rmtree(dir1_path)
  svntest.main.safe_rmtree(dir2_path)

  # Unschedule the additions, using 'svn rm' and 'svn revert'.
  svntest.main.run_svn(None, 'rm', file1_path, dir1_path)
  svntest.main.run_svn(None, 'revert', file2_path, dir2_path)

  # 'svn st' should now show absolutely zero local mods.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# Regression test for issue #962:
#
# Make sure 'rm foo; svn rm foo' works on files and directories.
# Also make sure that the deletion is committable.

def delete_missing(sbox):
  "schedule and commit deletion on missing items"

  sbox.build()
  wc_dir = sbox.wc_dir

  mu_path = os.path.join(wc_dir, 'A', 'mu')
  H_path = os.path.join(wc_dir, 'A', 'D', 'H')

  # Manually remove a file and a directory.
  os.remove(mu_path)
  svntest.main.safe_rmtree(H_path)

  # Now schedule them for deletion anyway, and make sure no error is output.
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', mu_path, H_path)

  # Commit the deletions.
  expected_output = svntest.wc.State(wc_dir, {
    'A/mu' : Item(verb='Deleting'),
    'A/D/H' : Item(verb='Deleting'),
    })

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.remove('A/mu', 'A/D/H',
                         'A/D/H/psi', 'A/D/H/omega', 'A/D/H/chi')
  expected_status.tweak(wc_rev=1)

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

#----------------------------------------------------------------------
# Regression test for issue #854:
# Revert . inside an svn added empty directory should generate an error.

def revert_inside_newly_added_dir(sbox):
  "revert inside a newly added dir"

  sbox.build()
  wc_dir = sbox.wc_dir
  
  was_cwd = os.getcwd()
  os.chdir(wc_dir)

  try:
    # Schedule a new directory for addition
    os.mkdir('foo')
    svntest.main.run_svn(None, 'add', 'foo')

    # Now change into the newly added directory, revert and make sure
    # an error is output.
    os.chdir('foo')
    svntest.actions.run_and_verify_svn(None, None, SVNAnyOutput,
                                       'revert', '.')
  finally:
    os.chdir(was_cwd)

#----------------------------------------------------------------------
# Regression test for issue #1609:
# 'svn status' should show a schedule-add directory as 'A' not '?'

def status_add_deleted_directory(sbox):
  "status after add of deleted directory"

  sbox.build()
  wc_dir = sbox.wc_dir

  # The original recipe:
  #
  # svnadmin create repo
  # svn mkdir file://`pwd`/repo/foo -m r1
  # svn co file://`pwd`/repo wc
  # svn rm wc/foo
  # rm -rf wc/foo
  # svn ci wc -m r2
  # svn mkdir wc/foo
  
  A_path = os.path.join(wc_dir, 'A')
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', A_path)
  svntest.main.safe_rmtree(A_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ci', '-m', 'log msg', wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', A_path)
  
  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status = svntest.wc.State(wc_dir,
                                     { ''     : Item(status='  ', wc_rev=1),
                                       'A'    : Item(status='A ', wc_rev=0),
                                       'iota' : Item(status='  ', wc_rev=1),
                                       })
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Update will *not* remove the entry for A despite it being marked
  # deleted.
  svntest.actions.run_and_verify_svn(None, ['At revision 2.\n'], [],
                                     'up', wc_dir)
  expected_status.tweak('', 'iota', wc_rev=2)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)


#----------------------------------------------------------------------
# Regression test for issue #939:
# Recursive 'svn add' should still traverse already-versioned dirs.
def add_recursive_already_versioned(sbox):
  "'svn add' should traverse already-versioned dirs"

  wc_dir = sbox.wc_dir

  if svntest.actions.make_repo_and_wc(sbox):
    return 1

  # Create some files, then schedule them for addition
  delta_path = os.path.join(wc_dir, 'delta')
  zeta_path = os.path.join(wc_dir, 'A', 'B', 'zeta')
  epsilon_path = os.path.join(wc_dir, 'A', 'D', 'G', 'epsilon')
  
  svntest.main.file_append(delta_path, "This is the file 'delta'.")
  svntest.main.file_append(zeta_path, "This is the file 'zeta'.")
  svntest.main.file_append(epsilon_path, "This is the file 'epsilon'.")
  
  saved_wd = os.getcwd()
  try:
    os.chdir(wc_dir)
    svntest.main.run_svn(None, 'add', '--force', '.')
  finally:
    os.chdir(saved_wd)
  
  # Make sure the adds show up as such in status
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'delta' : Item(status='A ', wc_rev=0),
    'A/B/zeta' : Item(status='A ', wc_rev=0),
    'A/D/G/epsilon' : Item(status='A ', wc_rev=0),
    })

  return svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# Regression test for the case where "svn mkdir" outside a working copy
# would create a directory, but then not clean up after itself when it
# couldn't add it to source control.
def fail_add_directory(sbox):
  "'svn mkdir' should clean up after itself on error"
  # This test doesn't use a working copy
  svntest.main.safe_rmtree(sbox.wc_dir)
  os.makedirs(sbox.wc_dir)
  saved_wd = os.getcwd()
  try:
    os.chdir(sbox.wc_dir)
    svntest.actions.run_and_verify_svn('Failed mkdir', None, SVNAnyOutput,
                                       'mkdir', 'A')
    if os.path.exists('A'):
      raise svntest.Failure('svn mkdir created an unversioned directory')
  finally:
    os.chdir(saved_wd)


########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              add_files,
              add_directories,
              nested_adds,
              Skip(add_executable, (os.name != 'posix')),
              delete_files,
              delete_dirs,
              revert_add_files,
              revert_add_directories,
              revert_nested_adds,
              Skip(revert_add_executable, (os.name != 'posix')),
              revert_delete_files,
              revert_delete_dirs,
              XFail(commit_add_files),
              XFail(commit_add_directories),
              XFail(commit_nested_adds),
              Skip(XFail(commit_add_executable), (os.name != 'posix')),
              XFail(commit_delete_files),
              XFail(commit_delete_dirs),
              unschedule_missing_added,
              delete_missing,
              revert_inside_newly_added_dir,
              status_add_deleted_directory,
              add_recursive_already_versioned,
              fail_add_directory,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

#!/usr/bin/env python
#
#  special_tests.py:  testing special file handling
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
import os, re

# Our testing module
import svntest


# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem

 
######################################################################
# Tests
#
#   Each test must return on success or raise on failure.


#----------------------------------------------------------------------

def general_symlink(sbox):
  "general symlink handling"

  sbox.build()
  wc_dir = sbox.wc_dir

  # First try to just commit a symlink
  newfile_path = os.path.join(wc_dir, 'newfile')
  linktarget_path = os.path.join(wc_dir, 'linktarget')
  svntest.main.file_append(linktarget_path, 'this is just a link target')
  os.symlink('linktarget', newfile_path)
  svntest.main.run_svn(None, 'add', newfile_path, linktarget_path)

  expected_output = svntest.wc.State(wc_dir, {
    'newfile' : Item(verb='Adding'),
    'linktarget' : Item(verb='Adding'),
    })

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.add({
    'newfile' : Item(status='  ', wc_rev=2, repos_rev=2),
    'linktarget' : Item(status='  ', wc_rev=2, repos_rev=2),
    })

  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  ## Now we should update to the previous version, verify that no
  ## symlink is present, then update back to HEAD and see if the symlink
  ## is regenerated properly.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'up', '-r', '1', wc_dir)

  # Is the symlink gone?
  if os.path.isfile(newfile_path) or os.path.islink(newfile_path):
    raise svntest.Failure
  
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'up', '-r', '2', wc_dir)
  
  # Is the symlink back?
  new_target = os.readlink(newfile_path)
  if new_target != 'linktarget':
    raise svntest.Failure

  ## Now change the target of the symlink, verify that it is shown as
  ## modified and that a commit succeeds.
  os.remove(newfile_path)
  os.symlink('A', newfile_path)

  was_cwd = os.getcwd()
  os.chdir(wc_dir)
  svntest.actions.run_and_verify_svn(None, [ "M      newfile\n" ], [], 'st')

  os.chdir(was_cwd)

  expected_output = svntest.wc.State(wc_dir, {
    'newfile' : Item(verb='Sending'),
    })

  expected_status = svntest.actions.get_virginal_state(wc_dir, 3)
  expected_status.tweak(wc_rev=2)
  expected_status.add({
    'newfile' : Item(status='  ', wc_rev=3, repos_rev=3),
    'linktarget' : Item(status='  ', wc_rev=2, repos_rev=3),
    })
  
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)


def replace_file_with_symlink(sbox):
  "replace a normal file with a special file"

  sbox.build()
  wc_dir = sbox.wc_dir

  # First replace a normal file with a symlink and make sure we get an
  # error
  iota_path = os.path.join(wc_dir, 'iota')
  os.remove(iota_path)
  os.symlink('A', iota_path)

  # Does status show the obstruction?
  was_cwd = os.getcwd()
  os.chdir(wc_dir)
  svntest.actions.run_and_verify_svn(None, [ "~      iota\n" ], [], 'st')

  # And does a commit fail?
  os.chdir(was_cwd)
  stdout_lines, stderr_lines = svntest.main.run_svn(1, 'ci', '-m',
                                                    'log msg', wc_dir)

  regex = 'svn: Commit failed'
  for line in stderr_lines:
    if re.match(regex, line):
      break
  else:
    raise svntest.Failure


  
########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              Skip(general_symlink, (os.name != 'posix')),
              Skip(replace_file_with_symlink, (os.name != 'posix')),
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

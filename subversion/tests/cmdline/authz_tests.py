#!/usr/bin/env python
#
#  authz_tests.py:  testing authentication.
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2006 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import os

# Our testing module
import svntest

from svntest.main import skip_test_when_no_authz_available
from svntest.main import write_restrictive_svnserve_conf 
from svntest.main import write_authz_file

# (abbreviation)
Item = svntest.wc.StateItem
XFail = svntest.testcase.XFail
Skip = svntest.testcase.Skip

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.


#----------------------------------------------------------------------

# regression test for issue #2486 - part 1: open_root

def authz_open_root(sbox):
  "authz issue #2486 - open root"
  
  skip_test_when_no_authz_available()
  
  sbox.build()
  
  write_authz_file(sbox, {"/": "", "/A": "jrandom = rw"})

  write_restrictive_svnserve_conf(sbox.repo_dir)

  # we have write access in folder /A, but not in root. Test on too
  # restrictive access needed in open_root by modifying a file in /A
  wc_dir = sbox.wc_dir
  
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  svntest.main.file_append(mu_path, "hi")
  
  # Create expected output tree.
  expected_output = svntest.wc.State(wc_dir, {
    'A/mu' : Item(verb='Sending'),
    })

  # Commit the one file.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        None,
                                        None,
                                        None, None,
                                        None, None,
                                        mu_path)

#----------------------------------------------------------------------

# regression test for issue #2486 - part 2: open_directory

def authz_open_directory(sbox):
  "authz issue #2486 - open directory"
  
  skip_test_when_no_authz_available()
  
  sbox.build()
  
  write_authz_file(sbox, {"/": "*=rw", "/A/B": "*=", "/A/B/E": "jrandom = rw"})
  
  write_restrictive_svnserve_conf(sbox.repo_dir) 

  # we have write access in folder /A/B/E, but not in /A/B. Test on too
  # restrictive access needed in open_directory by moving file /A/mu to
  # /A/B/E
  wc_dir = sbox.wc_dir
  
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')
  
  svntest.main.run_svn(None, 'mv', mu_path, E_path)
  
  # Create expected output tree.
  expected_output = svntest.wc.State(wc_dir, {
    'A/mu' : Item(verb='Deleting'),
    'A/B/E/mu' : Item(verb='Adding'),
    })
  
  # Commit the working copy.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        None,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

def broken_authz_file(sbox):
  "broken authz files cause errors"

  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)
  
  # No characters but 'r', 'w', and whitespace are allowed as a value
  # in an authz rule.
  write_authz_file(sbox, {"/": "jrandom = rw  # End-line comments disallowed"})
  
  write_restrictive_svnserve_conf(sbox.repo_dir)

  out, err = svntest.main.run_svn(1,
                                  "delete",
                                  "--username", svntest.main.wc_author,
                                  "--password", svntest.main.wc_passwd,
                                  sbox.repo_url + "/A",
                                  "-m", "a log message");
  if out:
    raise svntest.actions.SVNUnexpectedStdout(out)
  if not err:
    raise svntest.actions.SVNUnexpectedStderr("Missing stderr")

# test whether read access is correctly granted and denied
def authz_read_access(sbox):
  "test authz for read operations"
  
  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)

  root_url = sbox.repo_url
  A_url = root_url + '/A'
  B_url = A_url + '/B'
  C_url = A_url + '/C'
  E_url = B_url + '/E'
  mu_url = A_url + '/mu'
  iota_url = root_url + '/iota'
  lambda_url = B_url + '/lambda'
  alpha_url = E_url + '/alpha'
  D_url = A_url + '/D'
  G_url = D_url + '/G'
  pi_url = G_url + '/pi'
  H_url = D_url + '/H'
  chi_url = H_url + '/chi'

  if sbox.repo_url.startswith("http"):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Authorization failed.*"

  # create some folders with spaces in their names
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     B_url+'/folder with spaces',
                                     B_url+'/folder with spaces/empty folder')

  write_restrictive_svnserve_conf(sbox.repo_dir)

  write_authz_file(sbox, { "/": "* = r",
                           "/A/B": "* =",
                           "/A/D": "* = rw",
                           "/A/D/G": ("* = rw\n" +
                                      svntest.main.wc_author + " ="),
                           "/A/D/H": ("* = \n" +
                                      svntest.main.wc_author + " = rw"),
                           "/A/B/folder with spaces":
                                     (svntest.main.wc_author + " = r")})

  # read a remote file
  svntest.actions.run_and_verify_svn(None, ["This is the file 'iota'.\n"],
                                     [], 'cat',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     iota_url)

  # read a remote file, readably by user specific exception
  svntest.actions.run_and_verify_svn(None, ["This is the file 'chi'.\n"],
                                     [], 'cat',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     chi_url)
                                     
  # read a remote file, unreadable: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cat',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     lambda_url)

  # read a remote file, unreadable through recursion: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cat',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     alpha_url)

  # read a remote file, user specific authorization is ignored because * = rw
  svntest.actions.run_and_verify_svn(None, ["This is the file 'pi'.\n"],
                                     [], 'cat',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     pi_url)
  # open a remote folder(ls)
  svntest.actions.run_and_verify_svn("ls remote root folder",
                                     ["A/\n", "iota\n"],
                                     [], 'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     root_url)

  # open a remote folder(ls), unreadable: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, svntest.SVNAnyOutput, 'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B_url)

  # open a remote folder(ls) with spaces, should succeed
  svntest.actions.run_and_verify_svn(None,
                                     None, [], 'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B_url+'/folder with spaces/empty folder')

  # open a remote folder(ls), unreadable through recursion: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     E_url)

  # copy a remote file
  svntest.actions.run_and_verify_svn(None, None, [], 'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     iota_url, D_url,
                                     '-m', 'logmsg')

  # copy a remote file, source is unreadable: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     lambda_url, D_url)

  # copy a remote folder
  svntest.actions.run_and_verify_svn(None, None, [], 'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     C_url, D_url,
                                     '-m', 'logmsg')

  # copy a remote folder, source is unreadable: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     E_url, D_url)

# test whether write access is correctly granted and denied
def authz_write_access(sbox):
  "test authz for write operations"
  
  skip_test_when_no_authz_available()
  
  sbox.build(create_wc = False)
  
  write_restrictive_svnserve_conf(sbox.repo_dir)

  if sbox.repo_url.startswith('http'):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Access denied.*"

  write_authz_file(sbox, { "/": "* = r",
                           "/A/B": "* = rw",
                           "/A/C": "* = rw"})

  root_url = sbox.repo_url
  A_url = root_url + '/A'
  B_url = A_url + '/B'
  C_url = A_url + '/C'
  E_url = B_url + '/E'
  mu_url = A_url + '/mu'
  iota_url = root_url + '/iota'
  lambda_url = B_url + '/lambda'
  D_url = A_url + '/D'
  
  # copy a remote file, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     lambda_url, D_url)

  # copy a remote folder, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     E_url, D_url)

  # delete a file, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'rm',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     iota_url)

  # delete a folder, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'rm',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     D_url)

  # create a folder, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'mkdir',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     A_url+'/newfolder')

  # move a remote file, source is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'mv',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     mu_url, C_url)

  # move a remote folder, source is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'mv',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     D_url, C_url)

  # move a remote file, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'mv',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     lambda_url, D_url)

  # move a remote folder, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'mv',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     B_url, D_url)

#----------------------------------------------------------------------

def authz_checkout_test(sbox):
  "test authz for checkout"

  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)
  local_dir = sbox.wc_dir

  write_restrictive_svnserve_conf(sbox.repo_dir)

  # 1st part: disable all read access, checkout should fail
  
  # write an authz file with *= on /
  if sbox.repo_url.startswith('http'):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Authorization failed.*"
         
  write_authz_file(sbox, { "/": "* ="})
  
  # checkout a working copy, should fail
  svntest.actions.run_and_verify_svn(None, None, expected_err,
                                     'co', sbox.repo_url, local_dir)
                          
  # 2nd part: now enable read access
  
  write_authz_file(sbox, { "/": "* = r"})
         
  # checkout a working copy, should succeed because we have read access
  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = local_dir
  expected_output.tweak(status='A ', contents=None)

  expected_wc = svntest.main.greek_state
  
  svntest.actions.run_and_verify_checkout(sbox.repo_url,
                          local_dir,
                          expected_output,
                          expected_wc)

def authz_checkout_and_update_test(sbox):
  "test authz for checkout and update"

  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)
  local_dir = sbox.wc_dir

  write_restrictive_svnserve_conf(sbox.repo_dir)

  # 1st part: disable read access on folder A/B, checkout should not
  # download this folder
  
  # write an authz file with *= on /A/B
  write_authz_file(sbox, { "/": "* = r",
                           "/A/B": "* ="})
         
  # checkout a working copy, should not dl /A/B
  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = local_dir
  expected_output.tweak(status='A ', contents=None)
  expected_output.remove('A/B', 'A/B/lambda', 'A/B/E', 'A/B/E/alpha', 
                         'A/B/E/beta', 'A/B/F')
  
  expected_wc = svntest.main.greek_state.copy()
  expected_wc.remove('A/B', 'A/B/lambda', 'A/B/E', 'A/B/E/alpha', 
                     'A/B/E/beta', 'A/B/F')
  
  svntest.actions.run_and_verify_checkout(sbox.repo_url, local_dir, 
                                          expected_output,
                                          expected_wc)
  
  # 2nd part: now enable read access
  
  # write an authz file with *=r on /
  write_authz_file(sbox, { "/": "* = r"})
         
  # update the working copy, should download /A/B because we now have read
  # access
  expected_output = svntest.wc.State(local_dir, {
    'A/B' : Item(status='A '),
    'A/B/lambda' : Item(status='A '),
    'A/B/E' : Item(status='A '),
    'A/B/E/alpha' : Item(status='A '),
    'A/B/E/beta' : Item(status='A '),
    'A/B/F' : Item(status='A '),
    })

  expected_wc = svntest.main.greek_state
  expected_status = svntest.actions.get_virginal_state(local_dir, 1)

  svntest.actions.run_and_verify_update(local_dir,
                                        expected_output,
                                        expected_wc,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None, 1)
     
def authz_partial_export_test(sbox):
  "test authz for export with unreadable subfolder"

  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)
  local_dir = sbox.wc_dir

  # cleanup remains of a previous test run.
  svntest.main.safe_rmtree(local_dir)

  write_restrictive_svnserve_conf(sbox.repo_dir)

  # 1st part: disable read access on folder A/B, export should not
  # download this folder
  
  # write an authz file with *= on /A/B
  write_authz_file(sbox, { "/": "* = r", "/A/B": "* =" })
         
  # export a working copy, should not dl /A/B
  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = local_dir
  expected_output.desc[''] = Item()
  expected_output.tweak(status='A ', contents=None)
  expected_output.remove('A/B', 'A/B/lambda', 'A/B/E', 'A/B/E/alpha', 
                         'A/B/E/beta', 'A/B/F')
  
  expected_wc = svntest.main.greek_state.copy()
  expected_wc.remove('A/B', 'A/B/lambda', 'A/B/E', 'A/B/E/alpha', 
                     'A/B/E/beta', 'A/B/F')
  
  svntest.actions.run_and_verify_export(sbox.repo_url, local_dir, 
                                        expected_output,
                                        expected_wc)

#----------------------------------------------------------------------

def authz_log_and_tracing_test(sbox):
  "test authz for log and tracing path changes"

  skip_test_when_no_authz_available()

  sbox.build()
  wc_dir = sbox.wc_dir

  write_restrictive_svnserve_conf(sbox.repo_dir)

  # write an authz file with *=rw on /
  if sbox.repo_url.startswith('http'):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Authorization failed.*"

  write_authz_file(sbox, { "/": "* = rw\n" })
         
  root_url = sbox.repo_url
  D_url = root_url + '/A/D'
  G_url = D_url + '/G'
  
  # check if log doesn't spill any info on which you don't have read access
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  svntest.main.file_append(rho_path, 'new appended text for rho')
  
  svntest.actions.run_and_verify_svn(None, None, [],
                                 'ci', '-m', 'add file rho', sbox.wc_dir)

  svntest.main.file_append(rho_path, 'extra change in rho')

  svntest.actions.run_and_verify_svn(None, None, [],
                                 'ci', '-m', 'changed file rho', sbox.wc_dir)
  
  # copy a remote file
  svntest.actions.run_and_verify_svn(None, None, [], 'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     rho_path, D_url,
                                     '-m', 'copy rho to readable area')
                                                                                                       
  # now disable read access on the first version of rho, keep the copy in 
  # /A/D readable.
  if sbox.repo_url.startswith('http'):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Authorization failed.*"

  write_authz_file(sbox, { "/": "* = rw",
                           "/A/D/G": "* ="})
     
  ## log
  
  # changed file in this rev. is not readable anymore, so author and date
  # should be hidden, like this:
  # r2 | (no author) | (no date) | 1 line 
  svntest.actions.run_and_verify_svn(None, ".*(no author).*(no date).*", [],
                                     'log', '-r', '2', '--limit', '1',
                                     wc_dir)

  if sbox.repo_url.startswith('http'):
    expected_err2 = expected_err
  else:
    expected_err2 = ".*svn: Item is not readable.*"

  # if we do the same thing directly on the unreadable file, we get:
  # svn: Item is not readable
  svntest.actions.run_and_verify_svn(None, None, expected_err2,
                                     'log', rho_path)
                                     
  # while the HEAD rev of the copy is readable in /A/D, its parent in 
  # /A/D/G is not, so don't spill any info there either.
  svntest.actions.run_and_verify_svn(None, ".*(no author).*(no date).*", [],
                                    'log', '-r', '2', '--limit', '1', D_url)

  ## cat
  
  # now see if we can look at the older version of rho
  svntest.actions.run_and_verify_svn(None, None, expected_err,
                                    'cat', '-r', '2', D_url+'/rho')

  if sbox.repo_url.startswith('http'):
    expected_err2 = expected_err
  else:
    expected_err2 = ".*svn: Unreadable path encountered; access denied.*"

  svntest.actions.run_and_verify_svn(None, None, expected_err2,
                                    'cat', '-r', '2', G_url+'/rho')  
  
  ## diff
  
  # we shouldn't see the diff of a file in an unreadable path
  svntest.actions.run_and_verify_svn(None, None, expected_err,
                                    'diff', '-r', 'HEAD', G_url+'/rho')

  svntest.actions.run_and_verify_svn(None, None, expected_err,
                                    'diff', '-r', '2', D_url+'/rho')  

  svntest.actions.run_and_verify_svn(None, None, expected_err,
                                    'diff', '-r', '2:4', D_url+'/rho')  

# test whether read access is correctly granted and denied
def authz_aliases(sbox):
  "test authz for aliases"

  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)

  write_restrictive_svnserve_conf(sbox.repo_dir)

  if sbox.repo_url.startswith("http"):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Authorization failed.*"

  write_authz_file(sbox, { "/" : "* = r",
                           "/A/B" : "&jray = rw" },
                         { "aliases" : 'jray = jrandom' } )

  root_url = sbox.repo_url
  A_url = root_url + '/A'
  B_url = A_url + '/B'
  iota_url = root_url + '/iota'

  # copy a remote file, target is readonly for jconstant: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'cp',
                                     '--username', svntest.main.wc_author2,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     iota_url, B_url)

  # try the same action, but as user jray (alias of jrandom), should work.
  svntest.actions.run_and_verify_svn(None,
                                     None, [],
                                     'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'logmsg',
                                     iota_url, B_url)

def authz_validate(sbox):
  "test the authz validation rules"

  skip_test_when_no_authz_available()

  sbox.build(create_wc = False)

  write_restrictive_svnserve_conf(sbox.repo_dir)

  A_url = sbox.repo_url + '/A'

  # If any of the validate rules fail, the authz isn't loaded so there's no 
  # access at all to the repository.

  # Test 1: Undefined group
  write_authz_file(sbox, { "/"  : "* = r",
                           "/A/B" : "@undefined_group = rw" })

  if sbox.repo_url.startswith("http"):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*@undefined_group.*"

  # validation of this authz file should fail, so no repo access
  svntest.actions.run_and_verify_svn("ls remote folder",
                                     None, expected_err,
                                     'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url)

  # Test 2: Circular dependency
  write_authz_file(sbox, { "/"  : "* = r" },
                         { "groups" : """admins = admin1, admin2, @devs
devs1 = @admins, dev1
devs2 = @admins, dev2
devs = @devs1, dev3, dev4""" })

  if sbox.repo_url.startswith("http"):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*Circular dependency.*"

  # validation of this authz file should fail, so no repo access
  svntest.actions.run_and_verify_svn("ls remote folder",
                                     None, expected_err,
                                     'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url)

  # Test 3: Group including other group 2 times (issue 2684)
  write_authz_file(sbox, { "/"  : "* = r" },
                         { "groups" : """admins = admin1, admin2
devs1 = @admins, dev1
devs2 = @admins, dev2
users = @devs1, @devs2, user1, user2""" })

  # validation of this authz file should fail, so no repo access
  svntest.actions.run_and_verify_svn("ls remote folder",
                                      ['B/\n', 'C/\n', 'D/\n', 'mu\n'],
                                      [],
                                     'ls',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url)

# test locking/unlocking with authz
def authz_locking(sbox):
  "test authz for locking"

  skip_test_when_no_authz_available()

  sbox.build()

  write_authz_file(sbox, {"/": "", "/A": "jrandom = rw"})
  write_restrictive_svnserve_conf(sbox.repo_dir)

  if sbox.repo_url.startswith('http'):
    expected_err = ".*403 Forbidden.*"
  else:
    expected_err = ".*svn: Authorization failed.*"

  root_url = sbox.repo_url
  wc_dir = sbox.wc_dir
  iota_url = root_url + '/iota'
  iota_path = os.path.join(wc_dir, 'iota')
  A_url = root_url + '/A'
  mu_path = os.path.join(wc_dir, 'A', 'mu')

  # lock a file url, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'lock msg',
                                     iota_url)

  # lock a file path, target is readonly: should fail
  svntest.actions.run_and_verify_svn(None,
                                     None, expected_err,
                                     'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'lock msg',
                                     iota_path)

  # Test for issue 2700: we have write access in folder /A, but not in root. 
  # Get a lock on /A/mu and try to commit it.
 
  # lock a file path, target is writeable: should succeed
  svntest.actions.run_and_verify_svn(None,
                                     None, [],
                                     'lock',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'lock msg',
                                     mu_path)

  svntest.main.file_append(mu_path, "hi")

  expected_output = svntest.wc.State(wc_dir, {
    'A/mu' : Item(verb='Sending'),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        [],
                                        None,
                                        None, None,
                                        None, None,
                                        mu_path)
  
# test for issue #2712: if anon-access == read, svnserve should also check
# authz to determine whether a checkout/update is actually allowed for
# anonymous users, and, if not, attempt authentication.
def authz_svnserve_anon_access_read(sbox):
  "authz issue #2712"

  if not svntest.main.is_ra_type_svn():
    raise svntest.Skip

  sbox.build(create_wc = False)
  B_path = os.path.join(sbox.wc_dir, 'A', 'B')
  B_url = sbox.repo_url + '/A/B'
  D_path = os.path.join(sbox.wc_dir, 'A', 'D')
  D_url = sbox.repo_url + '/A/D'

  # We want a svnserve.conf with anon-access = read.
  write_restrictive_svnserve_conf(sbox.repo_dir, "read")

  # Give jrandom read access to /A/B.  Anonymous users can only
  # access /A/D.
  write_authz_file(sbox, { "/A/B" : "jrandom = r",
                           "/A/D" : "* = r" })

  # Perform a checkout of /A/B, expecting to see no errors.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'checkout',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B_url, B_path)

  # Anonymous users should be able to check out /A/D.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'checkout',
                                     D_url, D_path)

########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              authz_open_root,
              XFail(authz_open_directory, svntest.main.is_ra_type_dav),
              broken_authz_file,
              authz_read_access,
              authz_write_access,
              authz_checkout_test,
              authz_log_and_tracing_test,
              authz_checkout_and_update_test,
              authz_partial_export_test,
              authz_aliases,
              authz_validate,
              authz_locking,
              authz_svnserve_anon_access_read,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list, serial_only = True)
  # NOTREACHED


### End of file.

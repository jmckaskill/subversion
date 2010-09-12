#!/usr/bin/env python
#
#  mergeinfo_tests.py:  testing Merge Tracking reporting
#
#  Subversion is a tool for revision control.
#  See http://subversion.apache.org for more information.
#
# ====================================================================
#    Licensed to the Apache Software Foundation (ASF) under one
#    or more contributor license agreements.  See the NOTICE file
#    distributed with this work for additional information
#    regarding copyright ownership.  The ASF licenses this file
#    to you under the Apache License, Version 2.0 (the
#    "License"); you may not use this file except in compliance
#    with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing,
#    software distributed under the License is distributed on an
#    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#    KIND, either express or implied.  See the License for the
#    specific language governing permissions and limitations
#    under the License.
######################################################################

# General modules
import shutil, sys, re, os

# Our testing module
import svntest
from svntest import wc

# (abbreviation)
Item = wc.StateItem
XFail = svntest.testcase.XFail
Skip = svntest.testcase.Skip
SkipUnless = svntest.testcase.SkipUnless

from svntest.main import SVN_PROP_MERGEINFO
from svntest.main import server_has_mergeinfo

# Get a couple merge helpers from merge_tests.py
import merge_tests
from merge_tests import set_up_branch
from merge_tests import expected_merge_output

def adjust_error_for_server_version(expected_err):
  "Return the expected error regexp appropriate for the server version."
  if server_has_mergeinfo():
    return expected_err
  else:
    return ".*Retrieval of mergeinfo unsupported by '.+'"

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.


#----------------------------------------------------------------------

def no_mergeinfo(sbox):
  "'mergeinfo' on a URL that lacks mergeinfo"

  sbox.build(create_wc=False)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           [], sbox.repo_url, sbox.repo_url)

def mergeinfo(sbox):
  "'mergeinfo' on a path with mergeinfo"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Dummy up some mergeinfo.
  svntest.actions.run_and_verify_svn(None, None, [], 'ps', SVN_PROP_MERGEINFO,
                                     '/:1', wc_dir)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['1'], sbox.repo_url, wc_dir)

def explicit_mergeinfo_source(sbox):
  "'mergeinfo' with source selection"

  sbox.build()
  wc_dir = sbox.wc_dir
  H_path = os.path.join(wc_dir, 'A', 'D', 'H')
  H2_path = os.path.join(wc_dir, 'A', 'D', 'H2')
  B_url = sbox.repo_url + '/A/B'
  B_path = os.path.join(wc_dir, 'A', 'B')
  G_url = sbox.repo_url + '/A/D/G'
  G_path = os.path.join(wc_dir, 'A', 'D', 'G')
  H2_url = sbox.repo_url + '/A/D/H2'

  # Make a copy, and dummy up some mergeinfo.
  mergeinfo = '/A/B:1\n/A/D/G:1\n'
  svntest.actions.set_prop(SVN_PROP_MERGEINFO, mergeinfo, H_path)
  svntest.main.run_svn(None, "cp", H_path, H2_path)
  svntest.main.run_svn(None, "ci", "-m", "r2", wc_dir)

  # Check using each of our recorded merge sources (as paths and URLs).
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['1'], B_url, H_path)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['1'], B_path, H_path)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['1'], G_url, H_path)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['1'], G_path, H_path)

def mergeinfo_non_source(sbox):
  "'mergeinfo' with uninteresting source selection"

  sbox.build()
  wc_dir = sbox.wc_dir
  H_path = os.path.join(wc_dir, 'A', 'D', 'H')
  H2_path = os.path.join(wc_dir, 'A', 'D', 'H2')
  B_url = sbox.repo_url + '/A/B'
  B_path = os.path.join(wc_dir, 'A', 'B')
  G_url = sbox.repo_url + '/A/D/G'
  G_path = os.path.join(wc_dir, 'A', 'D', 'G')
  H2_url = sbox.repo_url + '/A/D/H2'

  # Make a copy, and dummy up some mergeinfo.
  mergeinfo = '/A/B:1\n/A/D/G:1\n'
  svntest.actions.set_prop(SVN_PROP_MERGEINFO, mergeinfo, H_path)
  svntest.main.run_svn(None, "cp", H_path, H2_path)
  svntest.main.run_svn(None, "ci", "-m", "r2", wc_dir)

  # Check on a source we haven't "merged" from.
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['2'], H2_url, H_path)

#----------------------------------------------------------------------
# Issue #3138
def mergeinfo_on_unknown_url(sbox):
  "mergeinfo of an unknown url should return error"

  sbox.build()
  wc_dir = sbox.wc_dir

  # remove a path from the repo and commit.
  iota_path = os.path.join(wc_dir, 'iota')
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', iota_path)
  svntest.actions.run_and_verify_svn("", None, [],
                                     "ci", wc_dir, "-m", "log message")

  url = sbox.repo_url + "/iota"
  expected_err = adjust_error_for_server_version(".*File not found.*iota.*|"
                                                 ".*iota.*path not found.*")
  svntest.actions.run_and_verify_svn("", None, expected_err,
                                     "mergeinfo", "--show-revs", "eligible",
                                     url, wc_dir)

# Test for issue #3126 'svn mergeinfo shows too few or too many
# eligible revisions'.  Specifically
# http://subversion.tigris.org/issues/show_bug.cgi?id=3126#desc5.
def non_inheritable_mergeinfo(sbox):
  "non-inheritable mergeinfo shows as merged"

  sbox.build()
  wc_dir = sbox.wc_dir
  expected_disk, expected_status = set_up_branch(sbox)

  # Some paths we'll care about
  A_COPY_path   = os.path.join(wc_dir, "A_COPY")
  D_COPY_path   = os.path.join(wc_dir, "A_COPY", "D")
  rho_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G", "rho")

  # Update the WC, then merge r4 from A to A_COPY and r6 from A to A_COPY
  # at --depth empty and commit the merges as r7.
  svntest.actions.run_and_verify_svn(None, ["At revision 6.\n"], [], 'up',
                                     wc_dir)
  expected_status.tweak(wc_rev=6)
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[4]],
                          ['U    ' + rho_COPY_path + '\n',
                           ' U   ' + A_COPY_path + '\n',]),
    [], 'merge', '-c4',
    sbox.repo_url + '/A',
    A_COPY_path)
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[6]], ' G   ' + A_COPY_path + '\n'),
    [], 'merge', '-c6',
    sbox.repo_url + '/A',
    A_COPY_path, '--depth', 'empty')
  expected_output = wc.State(wc_dir, {
    'A_COPY'         : Item(verb='Sending'),
    'A_COPY/D/G/rho' : Item(verb='Sending'),
    })
  expected_status.tweak('A_COPY', 'A_COPY/D/G/rho', wc_rev=7)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, wc_dir)

  # Update the WC a last time to ensure full inheritance.
  svntest.actions.run_and_verify_svn(None, ["At revision 7.\n"], [], 'up',
                                     wc_dir)

  # Despite being non-inheritable, r6 should still show as merged to A_COPY
  # and not eligible for merging.
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['4','6*'],
                                           sbox.repo_url + '/A',
                                           A_COPY_path)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['3','5','6*'],
                                           sbox.repo_url + '/A',
                                           A_COPY_path,
                                           '--show-revs', 'eligible')
  # But if we drop down to A_COPY/D, r6 should show as eligible because it
  # was only merged into A_COPY, no deeper.
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['4'],
                                           sbox.repo_url + '/A/D',
                                           D_COPY_path)
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['3','6'],
                                           sbox.repo_url + '/A/D',
                                           D_COPY_path,
                                           '--show-revs', 'eligible')

# Test for -R option with svn mergeinfo subcommand.
#
# Test for issue #3242 'Subversion demands unnecessary access to parent
# directories of operations'
def recursive_mergeinfo(sbox):
  "test svn mergeinfo -R"

  sbox.build()
  wc_dir = sbox.wc_dir
  expected_disk, expected_status = set_up_branch(sbox)

  # Some paths we'll care about
  A_path          = os.path.join(wc_dir, "A")
  A_COPY_path     = os.path.join(wc_dir, "A_COPY")
  B_COPY_path     = os.path.join(wc_dir, "A_COPY", "B")
  C_COPY_path     = os.path.join(wc_dir, "A_COPY", "C")
  rho_COPY_path   = os.path.join(wc_dir, "A_COPY", "D", "G", "rho")
  H_COPY_path     = os.path.join(wc_dir, "A_COPY", "D", "H")
  F_COPY_path     = os.path.join(wc_dir, "A_COPY", "B", "F")
  omega_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "H", "omega")
  beta_COPY_path  = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  A2_path         = os.path.join(wc_dir, "A2")
  nu_path         = os.path.join(wc_dir, "A2", "B", "F", "nu")
  nu_COPY_path    = os.path.join(wc_dir, "A_COPY", "B", "F", "nu")
  nu2_path        = os.path.join(wc_dir, "A2", "C", "nu2")

  # Rename A to A2 in r7.
  svntest.actions.run_and_verify_svn(None, ["At revision 6.\n"], [], 'up', wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ren', A_path, A2_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ci', wc_dir, '-m', 'rename A to A2')

  # Add the files A/B/F/nu and A/C/nu2 and commit them as r8.
  svntest.main.file_write(nu_path, "A new file.\n")
  svntest.main.file_write(nu2_path, "Another new file.\n")
  svntest.main.run_svn(None, "add", nu_path, nu2_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ci', wc_dir, '-m', 'Add 2 new files')

  # Do several merges to create varied subtree mergeinfo

  # Merge r4 from A2 to A_COPY at depth empty
  svntest.actions.run_and_verify_svn(None, ["At revision 8.\n"], [], 'up',
                                     wc_dir)
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[4]], ' U   ' + A_COPY_path + '\n'),
    [], 'merge', '-c4', '--depth', 'empty',
    sbox.repo_url + '/A2',
    A_COPY_path)

  # Merge r6 from A2/D/H to A_COPY/D/H
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[6]],
                          ['U    ' + omega_COPY_path + '\n',
                           ' G   ' + H_COPY_path + '\n']),
    [], 'merge', '-c6',
    sbox.repo_url + '/A2/D/H',
    H_COPY_path)

  # Merge r5 from A2 to A_COPY
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[5]],
                          ['U    ' + beta_COPY_path + '\n',
                           ' G   ' + A_COPY_path + '\n',
                           ' G   ' + B_COPY_path + '\n',
                           ' U   ' + B_COPY_path + '\n',], # Elision
                          elides=True),
    [], 'merge', '-c5',
    sbox.repo_url + '/A2',
    A_COPY_path)

  # Reverse merge -r5 from A2/C to A_COPY/C leaving empty mergeinfo on
  # A_COPY/C.
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[-5]],
                          ' G   ' + C_COPY_path + '\n'),
    [], 'merge', '-c-5',
    sbox.repo_url + '/A2/C', C_COPY_path)

  # Merge r8 from A2/B/F to A_COPY/B/F
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[8]],
                          ['A    ' + nu_COPY_path + '\n',
                           ' G   ' + F_COPY_path + '\n']),
    [], 'merge', '-c8',
    sbox.repo_url + '/A2/B/F',
    F_COPY_path)

  # Commit everything this far as r9
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ci', wc_dir, '-m', 'Many merges')
  svntest.actions.run_and_verify_svn(None, ["At revision 9.\n"], [], 'up',
                                     wc_dir)

  # Test svn mergeinfo -R / --depth infinity.

  # Asking for eligible revisions from A2 to A_COPY should show:
  #
  #  r3  - Was never merged.
  #
  #  r4 - Was merged at depth empty, so while there is mergeinfo for the
  #       revision, the actual text change to A_COPY/D/G/rho hasn't yet
  #       happened.
  #
  #  r8* - Was only partially merged to the subtree at A_COPY/B/F.  The
  #        addition of A_COPY/C/nu2 is still outstanding.
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['3', '4*', '8*'],
                                           sbox.repo_url + '/A2',
                                           sbox.repo_url + '/A_COPY',
                                           '--show-revs', 'eligible', '-R')

  # Asking for merged revisions from A2 to A_COPY should show:
  #
  #  r4* - Was merged at depth empty, so while there is mergeinfo for the
  #        revision, the actual text change to A_COPY/D/G/rho hasn't yet
  #        happened.
  #
  #  r5  - Was merged at depth infinity to the root of the 'branch', so it
  #        should show as fully merged.
  #
  #  r6  - This was a subtree merge, but since the subtree A_COPY/D/H was
  #        the ancestor of the only change made in r6 it is considered
  #        fully merged.
  #
  #  r8* - Was only partially merged to the subtree at A_COPY/B/F.  The
  #        addition of A_COPY/C/nu2 is still outstanding.
  svntest.actions.run_and_verify_mergeinfo(adjust_error_for_server_version(""),
                                           ['4*', '5', '6', '8*'],
                                           A2_path,
                                           A_COPY_path,
                                           '--show-revs', 'merged',
                                           '--depth', 'infinity')

  # A couple tests of problems found with initial issue #3242 fixes.
  # We should be able to check for the merged revs from a URL to a URL
  # when the latter has explicit mergeinfo...
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''), ['6'],
    sbox.repo_url + '/A2/D/H',
    sbox.repo_url + '/A_COPY/D/H',
    '--show-revs', 'merged')
  # ...and when the latter has inherited mergeinfo.
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''), ['6'],
    sbox.repo_url + '/A2/D/H/omega',
    sbox.repo_url + '/A_COPY/D/H/omega',
    '--show-revs', 'merged')
  
# Test for issue #3180 'svn mergeinfo ignores peg rev for WC target'.
def mergeinfo_on_pegged_wc_path(sbox):
  "svn mergeinfo on pegged working copy target"

  sbox.build()
  wc_dir = sbox.wc_dir
  expected_disk, expected_status = set_up_branch(sbox)

  # Some paths we'll care about
  A_path          = os.path.join(wc_dir, "A")
  A_COPY_path     = os.path.join(wc_dir, "A_COPY")
  psi_COPY_path   = os.path.join(wc_dir, "A_COPY", "D", "H", "psi")
  omega_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "H", "omega")
  beta_COPY_path  = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")

  # Do a couple merges
  #
  # r7 - Merge -c3,6 from A to A_COPY.
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[3],[6]],
                          ['U    ' + psi_COPY_path + '\n',
                           'U    ' + omega_COPY_path + '\n',
                           ' U   ' + A_COPY_path + '\n',
                           ' G   ' + A_COPY_path + '\n',]),
    [], 'merge', '-c3,6', sbox.repo_url + '/A', A_COPY_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ci', wc_dir,
                                     '-m', 'Merge r3 and r6')

  # r8 - Merge -c5 from A to A_COPY.
  svntest.actions.run_and_verify_svn(
    None,
    expected_merge_output([[5]],
                          ['U    ' + beta_COPY_path + '\n',
                           ' U   ' + A_COPY_path + '\n']),
    [], 'merge', '-c5', sbox.repo_url + '/A', A_COPY_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'ci', wc_dir,
                                     '-m', 'Merge r5')

  # Ask for merged and eligible revisions to A_COPY pegged at various values.
  # Prior to issue #3180 fix the peg revision was ignored.
  #
  # A_COPY pegged to non-existent revision
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version('.*No such revision 99'),
    [], A_path, A_COPY_path + '@99', '--show-revs', 'merged')

  # A_COPY@BASE
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['3','5','6'], A_path, A_COPY_path + '@BASE', '--show-revs', 'merged')

  # A_COPY@HEAD
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['3','5','6'], A_path, A_COPY_path + '@HEAD', '--show-revs', 'merged')

  # A_COPY@4 (Prior to any merges)
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    [], A_path, A_COPY_path + '@4', '--show-revs', 'merged')

  # A_COPY@COMMITTED (r8)
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['3','5','6'], A_path, A_COPY_path + '@COMMITTED', '--show-revs',
    'merged')

  # A_COPY@PREV (r7)
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['3', '6'], A_path, A_COPY_path + '@PREV', '--show-revs', 'merged')

  # A_COPY@BASE
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['4'], A_path, A_COPY_path + '@BASE', '--show-revs', 'eligible')

  # A_COPY@HEAD
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['4'], A_path, A_COPY_path + '@HEAD', '--show-revs', 'eligible')

  # A_COPY@4 (Prior to any merges)
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['3', '4', '5', '6'], A_path, A_COPY_path + '@4', '--show-revs', 'eligible')

  # A_COPY@COMMITTED (r8)
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['4'], A_path, A_COPY_path + '@COMMITTED', '--show-revs',
    'eligible')

  # A_COPY@PREV (r7)
  svntest.actions.run_and_verify_mergeinfo(
    adjust_error_for_server_version(''),
    ['4', '5'], A_path, A_COPY_path + '@PREV', '--show-revs', 'eligible')

########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              no_mergeinfo,
              mergeinfo,
              SkipUnless(explicit_mergeinfo_source, server_has_mergeinfo),
              XFail(mergeinfo_non_source, server_has_mergeinfo),
              mergeinfo_on_unknown_url,
              non_inheritable_mergeinfo,
              SkipUnless(recursive_mergeinfo, server_has_mergeinfo),
              SkipUnless(mergeinfo_on_pegged_wc_path,
                         server_has_mergeinfo),
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED

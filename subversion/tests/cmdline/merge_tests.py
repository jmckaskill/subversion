#!/usr/bin/env python
#
#  merge_tests.py:  testing merge
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2000-2007 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import shutil, sys, re, os
import time

# Our testing module
import svntest
from svntest import wc

# (abbreviation)
Item = wc.StateItem
XFail = svntest.testcase.XFail
Skip = svntest.testcase.Skip
SkipUnless = svntest.testcase.SkipUnless

from svntest.main import SVN_PROP_MERGE_INFO
from svntest.main import write_restrictive_svnserve_conf
from svntest.main import write_authz_file
from svntest.main import server_has_mergeinfo
from svntest.actions import fill_file_with_lines
from svntest.actions import make_conflict_marker_text
from svntest.actions import inject_conflict_into_expected_state

def shorten_path_kludge(path):
  '''Search for the comment entitled "The Merge Kluge" elsewhere in
  this file, to understand why we shorten, and subsequently chdir()
  after calling this function.'''
  shorten_by = len(svntest.main.work_dir) + len(os.sep)
  return path[shorten_by:]

def expected_merge_output(start_rev, additional_lines=None, end_rev=None,
                          same_URL=True):
  """Generate an (inefficient) regex representing the expected merge
  output from START_REV, ADDITIONAL_LINES (an array of strings), and
  END_REV."""
  lines = [svntest.main.merge_notify_line(start_rev, end_rev, same_URL)]
  if isinstance(additional_lines, list):
    # Address "The Backslash Plague"
    #
    # If ADDITIONAL_LINES are present there are possibly paths in it with
    # multiple components and on Windows these components are separated with
    # '\'.  These need to be escaped properly in the regexp for the match to
    # work correctly.  See http://aspn.activestate.com/ASPN/docs/ActivePython
    # /2.2/howto/regex/regex.html#SECTION000420000000000000000.
    if sys.platform == 'win32':
      for i in range(0, len(additional_lines)):
        additional_lines[i] = additional_lines[i].replace("\\", "\\\\")
    lines.extend(additional_lines)
  else:
    if sys.platform == 'win32' and additional_lines != None:
      additional_lines = additional_lines.replace("\\", "\\\\")
    lines.append(str(additional_lines))
  return "|".join(lines)

def expected_merge_output3(rev_ranges, additional_lines=None, same_URL=True):
  """Generate an (inefficient) regex representing the expected merge
  output from START_REV, ADDITIONAL_LINES (an array of strings), and
  END_REV."""
  lines = []
  for rng in rev_ranges:
    start_rev = rng[0]
    if len(rng) > 1:
      end_rev = rng[1]
    else:
      end_rev = None
    lines += [svntest.main.merge_notify_line(start_rev, end_rev, same_URL)]
  if isinstance(additional_lines, list): 
    # Address "The Backslash Plague"
    #
    # If ADDITIONAL_LINES are present there are possibly paths in it with
    # multiple components and on Windows these components are separated with
    # '\'.  These need to be escaped properly in the regexp for the match to
    # work correctly.  See http://aspn.activestate.com/ASPN/docs/ActivePython
    # /2.2/howto/regex/regex.html#SECTION000420000000000000000.
    if sys.platform == 'win32':
      for i in range(0, len(additional_lines)):
        additional_lines[i] = additional_lines[i].replace("\\", "\\\\")
    lines.extend(additional_lines)
  else:
    if sys.platform == 'win32' and additional_lines != None:
      additional_lines = additional_lines.replace("\\", "\\\\")
    lines.append(str(additional_lines))
  return "|".join(lines)

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.


#----------------------------------------------------------------------

def textual_merges_galore(sbox):
  "performing a merge, with mixed results"

  ## The Plan:
  ##
  ## The goal is to test that "svn merge" does the right thing in the
  ## following cases:
  ##
  ##   1 : _ :  Received changes already present in unmodified local file
  ##   2 : U :  No local mods, received changes folded in without trouble
  ##   3 : G :  Received changes already exist as local mods
  ##   4 : G :  Received changes do not conflict with local mods
  ##   5 : C :  Received changes conflict with local mods
  ##
  ## So first modify these files and commit:
  ##
  ##    Revision 2:
  ##    -----------
  ##    A/mu ............... add ten or so lines
  ##    A/D/G/rho .......... add ten or so lines
  ##
  ## Now check out an "other" working copy, from revision 2.
  ##
  ## Next further modify and commit some files from the original
  ## working copy:
  ##
  ##    Revision 3:
  ##    -----------
  ##    A/B/lambda ......... add ten or so lines
  ##    A/D/G/pi ........... add ten or so lines
  ##    A/D/G/tau .......... add ten or so lines
  ##    A/D/G/rho .......... add an additional ten or so lines
  ##
  ## In the other working copy (which is at rev 2), update rho back
  ## to revision 1, while giving other files local mods.  This sets
  ## things up so that "svn merge -r 1:3" will test all of the above
  ## cases except case 4:
  ##
  ##    case 1: A/mu .......... do nothing, the only change was in rev 2
  ##    case 2: A/B/lambda .... do nothing, so we accept the merge easily
  ##    case 3: A/D/G/pi ...... add same ten lines as committed in rev 3
  ##    case 5: A/D/G/tau ..... add ten or so lines at the end
  ##    [none]: A/D/G/rho ..... ignore what happens to this file for now
  ##
  ## Now run
  ##
  ##    $ cd wc.other
  ##    $ svn merge -r 1:3 url-to-repo
  ##
  ## ...and expect the right output.
  ##
  ## Now revert rho, then update it to revision 2, then *prepend* a
  ## bunch of lines, which will be separated by enough distance from
  ## the changes about to be received that the merge will be clean.
  ##
  ##    $ cd wc.other/A/D/G
  ##    $ svn merge -r 2:3 url-to-repo/A/D/G
  ##
  ## Which tests case 4.  (Ignore the changes to the other files,
  ## we're only interested in rho here.)

  sbox.build()
  wc_dir = sbox.wc_dir
  #  url = os.path.join(svntest.main.test_area_url, sbox.repo_dir)

  # Change mu and rho for revision 2
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  mu_text = fill_file_with_lines(mu_path, 2)
  rho_text = fill_file_with_lines(rho_path, 2)

  # Create expected output tree for initial commit
  expected_output = wc.State(wc_dir, {
    'A/mu' : Item(verb='Sending'),
    'A/D/G/rho' : Item(verb='Sending'),
    })

  # Create expected status tree; all local revisions should be at 1,
  # but mu and rho should be at revision 2.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', 'A/D/G/rho', wc_rev=2)

  # Initial commit.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Make the "other" working copy
  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  # Now commit some more mods from the original working copy, to
  # produce revision 3.
  lambda_path = os.path.join(wc_dir, 'A', 'B', 'lambda')
  pi_path = os.path.join(wc_dir, 'A', 'D', 'G', 'pi')
  tau_path = os.path.join(wc_dir, 'A', 'D', 'G', 'tau')

  lambda_text = fill_file_with_lines(lambda_path, 2)
  pi_text = fill_file_with_lines(pi_path, 2)
  tau_text = fill_file_with_lines(tau_path, 2)
  additional_rho_text = fill_file_with_lines(rho_path, 2)

  # Created expected output tree for 'svn ci'
  expected_output = wc.State(wc_dir, {
    'A/B/lambda' : Item(verb='Sending'),
    'A/D/G/pi' : Item(verb='Sending'),
    'A/D/G/tau' : Item(verb='Sending'),
    'A/D/G/rho' : Item(verb='Sending'),
    })

  # Create expected status tree.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', wc_rev=2)
  expected_status.tweak('A/B/lambda', 'A/D/G/pi', 'A/D/G/tau', 'A/D/G/rho',
                        wc_rev=3)

  # Commit revision 3.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Make local mods in wc.other
  other_pi_path = os.path.join(other_wc, 'A', 'D', 'G', 'pi')
  other_rho_path = os.path.join(other_wc, 'A', 'D', 'G', 'rho')
  other_tau_path = os.path.join(other_wc, 'A', 'D', 'G', 'tau')

  # For A/mu and A/B/lambda, we do nothing.  For A/D/G/pi, we add the
  # same ten lines as were already committed in revision 3.
  # (Remember, wc.other is only at revision 2, so it doesn't have
  # these changes.)
  svntest.main.file_append(other_pi_path, pi_text)

  # We skip A/D/G/rho in this merge; it will be tested with a separate
  # merge command.  Temporarily put it back to revision 1, so this
  # merge succeeds cleanly.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'up', '-r', '1', other_rho_path)

  # For A/D/G/tau, we append ten different lines, to conflict with the
  # ten lines appended in revision 3.
  other_tau_text = fill_file_with_lines(other_tau_path, 2,
                                        line_descrip="Conflicting line")

  # Do the first merge, revs 1:3.  This tests all the cases except
  # case 4, which we'll handle in a second pass.
  expected_output = wc.State(other_wc, {'A/B/lambda' : Item(status='U '),
                                        'A/D/G/rho'  : Item(status='U '),
                                        'A/D/G/tau'  : Item(status='C '),
                                        })

  expected_disk = svntest.main.greek_state.copy()
  expected_disk.tweak('A/mu',
                      contents=expected_disk.desc['A/mu'].contents
                      + mu_text)
  expected_disk.tweak('A/B/lambda',
                      contents=expected_disk.desc['A/B/lambda'].contents
                      + lambda_text)
  expected_disk.tweak('A/D/G/rho',
                      contents=expected_disk.desc['A/D/G/rho'].contents
                      + rho_text + additional_rho_text)
  expected_disk.tweak('A/D/G/pi',
                      contents=expected_disk.desc['A/D/G/pi'].contents
                      + pi_text)

  expected_status = svntest.actions.get_virginal_state(other_wc, 1)
  expected_status.tweak('', status=' M')
  expected_status.tweak('A/mu', wc_rev=2)
  expected_status.tweak('A/B/lambda', status='M ')
  expected_status.tweak('A/D/G/pi', status='M ')
  expected_status.tweak('A/D/G/rho', status='M ')

  inject_conflict_into_expected_state('A/D/G/tau', expected_disk,
                                      expected_status, other_tau_text, tau_text,
                                      3)

  expected_skip = wc.State('', { })

  tau_conflict_support_files = ["tau\.working",
                                "tau\.merge-right\.r3",
                                "tau\.merge-left\.r1"]

  svntest.actions.run_and_verify_merge(other_wc, '1', '3',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None,
                                       svntest.tree.detect_conflict_files,
                                       list(tau_conflict_support_files))

  # Now reverse merge r3 into A/D/G/rho, give it non-conflicting local
  # mods, then merge in the 2:3 change.  ### Not bothering to do the
  # whole expected_foo routine for these intermediate operations;
  # they're not what we're here to test, after all, so it's enough to
  # know that they worked.  Is this a bad practice? ###
  #
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-3, 'G    ' +
                                                           other_rho_path +
                                                           '\n'),
                                     [], 'merge', '-c-3',
                                     sbox.repo_url + '/A/D/G/rho',
                                     other_rho_path)

  # Now *prepend* ten or so lines to A/D/G/rho.  Since rho had ten
  # lines appended in revision 2, and then another ten in revision 3,
  # these new local mods will be separated from the rev 3 changes by
  # enough distance that they won't conflict, so the merge should be
  # clean.
  other_rho_text = ""
  for x in range(1,10):
    other_rho_text = other_rho_text + 'Unobtrusive line ' + `x` + ' in rho\n'
  current_other_rho_text = svntest.main.file_read(other_rho_path)
  svntest.main.file_write(other_rho_path,
                          other_rho_text + current_other_rho_text)

  # We expect no merge attempt for pi and tau because they inherit
  # mergeinfo from the WC root.  There is explicit mergeinfo on rho
  # ('/A/D/G/rho:2') so expect it to be merged (cleanly).
  expected_output = wc.State(os.path.join(other_wc, 'A', 'D', 'G'),
                             {'rho' : Item(status='G ')})
  expected_disk = wc.State("", {
    'pi'    : Item("This is the file 'pi'.\n"),
    'rho'   : Item("This is the file 'rho'.\n"),
    'tau'   : Item("This is the file 'tau'.\n"),
    })
  expected_disk.tweak('rho',
                      contents=other_rho_text
                      + expected_disk.desc['rho'].contents
                      + rho_text
                      + additional_rho_text)
  expected_disk.tweak('pi',
                      contents=expected_disk.desc['pi'].contents
                      + pi_text)

  expected_status = wc.State(os.path.join(other_wc, 'A', 'D', 'G'),
                             { ''     : Item(wc_rev=1, status='  '),
                               'rho'  : Item(wc_rev=1, status='M '),
                               'pi'   : Item(wc_rev=1, status='M '),
                               'tau'  : Item(wc_rev=1, status='C '),
                               })

  inject_conflict_into_expected_state('tau', expected_disk, expected_status,
                                      other_tau_text, tau_text, 3)

  # Do the merge, but check svn:mergeinfo props separately since
  # run_and_verify_merge would attempt to proplist tau's conflict
  # files if we asked it to check props.
  svntest.actions.run_and_verify_merge(
    os.path.join(other_wc, 'A', 'D', 'G'),
    '2', '3',
    sbox.repo_url + '/A/D/G',
    expected_output,
    expected_disk,
    expected_status,
    expected_skip,
    None,
    svntest.tree.detect_conflict_files, list(tau_conflict_support_files))


  svntest.actions.run_and_verify_svn(None, [], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     os.path.join(other_wc,
                                                  "A", "D", "G", "rho"))


#----------------------------------------------------------------------

# Merge should copy-with-history when adding files or directories

def add_with_history(sbox):
  "merge and add new files/dirs with history"

  sbox.build()
  wc_dir = sbox.wc_dir

  C_path = os.path.join(wc_dir, 'A', 'C')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'

  Q_path = os.path.join(F_path, 'Q')
  Q2_path = os.path.join(F_path, 'Q2')
  foo_path = os.path.join(F_path, 'foo')
  foo2_path = os.path.join(F_path, 'foo2')
  bar_path = os.path.join(F_path, 'Q', 'bar')
  bar2_path = os.path.join(F_path, 'Q', 'bar2')

  svntest.main.run_svn(None, 'mkdir', Q_path)
  svntest.main.run_svn(None, 'mkdir', Q2_path)
  svntest.main.file_append(foo_path, "foo")
  svntest.main.file_append(foo2_path, "foo2")
  svntest.main.file_append(bar_path, "bar")
  svntest.main.file_append(bar2_path, "bar2")
  svntest.main.run_svn(None, 'add', foo_path, foo2_path, bar_path, bar2_path)
  svntest.main.run_svn(None, 'propset', 'x', 'x', Q2_path)
  svntest.main.run_svn(None, 'propset', 'y', 'y', foo2_path)
  svntest.main.run_svn(None, 'propset', 'z', 'z', bar2_path)

  expected_output = wc.State(wc_dir, {
    'A/B/F/Q'     : Item(verb='Adding'),
    'A/B/F/Q2'    : Item(verb='Adding'),
    'A/B/F/Q/bar' : Item(verb='Adding'),
    'A/B/F/Q/bar2': Item(verb='Adding'),
    'A/B/F/foo'   : Item(verb='Adding'),
    'A/B/F/foo2'  : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/Q'     : Item(status='  ', wc_rev=2),
    'A/B/F/Q2'    : Item(status='  ', wc_rev=2),
    'A/B/F/Q/bar' : Item(status='  ', wc_rev=2),
    'A/B/F/Q/bar2': Item(status='  ', wc_rev=2),
    'A/B/F/foo'   : Item(status='  ', wc_rev=2),
    'A/B/F/foo2'  : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  ### "The Merge Kluge"
  ###
  ###      *****************************************************
  ###      ***                                               ***
  ###      ***   Before erasing this comment, please check   ***
  ###      ***      for references to "The Merge Kluge"      ***
  ###      ***   elsewhere in this file, update_tests.py     ***
  ###      ***              and switch_tests.py.             ***
  ###      ***                                               ***
  ###      *****************************************************
  ###
  ### The shortening of C_path and the chdir() below are a kluge to
  ### work around
  ###
  ###   http://subversion.tigris.org/issues/show_bug.cgi?id=767#desc16
  ###
  ### Note that the problem isn't simply that 'svn merge' sometimes
  ### puts temp files in cwd.  That's bad enough, but even if svn
  ### were to choose /tmp or some other static place blessed by
  ### apr_get_temp_dir(), we'd still experience the error
  ###
  ###   svn: Move failed
  ###   svn: Can't move 'tmp.2' to '.../.svn/tmp/text-base/file1.svn-base':
  ###        Invalid cross-device link
  ###
  ### when running the tests on a ramdisk.  After all, there's no
  ### reason why apr_get_temp_dir() would return a path inside
  ### svn-test-work/, which is the mount point for the ramdisk.
  ###
  ### http://subversion.tigris.org/issues/show_bug.cgi?id=767#desc20
  ### starts a discussion on how to solve this in Subversion itself.
  ### However, until that's settled, we still want to be able to run
  ### the tests in a ramdisk, hence this kluge.

  short_C_path = shorten_path_kludge(C_path)
  expected_output = wc.State(short_C_path, {
    'Q'      : Item(status='A '),
    'Q2'     : Item(status='A '),
    'Q/bar'  : Item(status='A '),
    'Q/bar2' : Item(status='A '),
    'foo'    : Item(status='A '),
    'foo2'   : Item(status='A '),
    })
  expected_disk = wc.State('', {
    ''       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:2'}),
    'Q'      : Item(),
    'Q2'     : Item(props={'x' : 'x'}),
    'Q/bar'  : Item("bar"),
    'Q/bar2' : Item("bar2", props={'z' : 'z'}),
    'foo'    : Item("foo"),
    'foo2'   : Item("foo2", props={'y' : 'y'}),
    })
  expected_status = wc.State(short_C_path, {
    ''       : Item(status=' M', wc_rev=1),
    'Q'      : Item(status='A ', wc_rev='-', copied='+'),
    'Q2'     : Item(status='A ', wc_rev='-', copied='+'),
    'Q/bar'  : Item(status='A ', wc_rev='-', copied='+'),
    'Q/bar2' : Item(status='A ', wc_rev='-', copied='+'),
    'foo'    : Item(status='A ', wc_rev='-', copied='+'),
    'foo2'   : Item(status='A ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(short_C_path, { })

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_C_path, '1', '2', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1) # check props
  os.chdir(saved_cwd)

  expected_output = svntest.wc.State(wc_dir, {
    'A/C'       : Item(verb='Sending'),
    'A/C/Q'     : Item(verb='Adding'),
    'A/C/Q2'    : Item(verb='Adding'),
    'A/C/Q/bar' : Item(verb='Adding'),
    'A/C/Q/bar2': Item(verb='Adding'),
    'A/C/foo'   : Item(verb='Adding'),
    'A/C/foo2'  : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/C'         : Item(status='  ', wc_rev=3),
    'A/B/F/Q'     : Item(status='  ', wc_rev=2),
    'A/B/F/Q2'    : Item(status='  ', wc_rev=2),
    'A/B/F/Q/bar' : Item(status='  ', wc_rev=2),
    'A/B/F/Q/bar2': Item(status='  ', wc_rev=2),
    'A/B/F/foo'   : Item(status='  ', wc_rev=2),
    'A/B/F/foo2'  : Item(status='  ', wc_rev=2),
    'A/C/Q'       : Item(status='  ', wc_rev=3),
    'A/C/Q2'      : Item(status='  ', wc_rev=3),
    'A/C/Q/bar'   : Item(status='  ', wc_rev=3),
    'A/C/Q/bar2'  : Item(status='  ', wc_rev=3),
    'A/C/foo'     : Item(status='  ', wc_rev=3),
    'A/C/foo2'    : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

#----------------------------------------------------------------------

def delete_file_and_dir(sbox):
  "merge that deletes items"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Rev 2 copy B to B2
  B_path = os.path.join(wc_dir, 'A', 'B')
  B2_path = os.path.join(wc_dir, 'A', 'B2')
  B_url = sbox.repo_url + '/A/B'

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'copy', B_path, B2_path)

  expected_output = wc.State(wc_dir, {
    'A/B2'       : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B2'         : Item(status='  ', wc_rev=2),
    'A/B2/E'       : Item(status='  ', wc_rev=2),
    'A/B2/E/alpha' : Item(status='  ', wc_rev=2),
    'A/B2/E/beta'  : Item(status='  ', wc_rev=2),
    'A/B2/F'       : Item(status='  ', wc_rev=2),
    'A/B2/lambda'  : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Rev 3 delete E and lambda from B
  E_path = os.path.join(B_path, 'E')
  lambda_path = os.path.join(B_path, 'lambda')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'delete', E_path, lambda_path)

  expected_output = wc.State(wc_dir, {
    'A/B/E'       : Item(verb='Deleting'),
    'A/B/lambda'       : Item(verb='Deleting'),
    })
  expected_status.remove('A/B/E',
                         'A/B/E/alpha',
                         'A/B/E/beta',
                         'A/B/lambda')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Local mods in B2
  B2_E_path = os.path.join(B2_path, 'E')
  B2_lambda_path = os.path.join(B2_path, 'lambda')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     B2_E_path, B2_lambda_path)
  expected_status.tweak(
    'A/B2/E', 'A/B2/lambda',  status=' M'
    )
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Merge rev 3 into B2

  # The local mods to the paths modified in r3 cause the paths to be
  # skipped (without --force), resulting in no changes to the WC.
  expected_output = wc.State(B2_path, { })
  expected_disk = wc.State('', {
    'E'       : Item(),
    'E/alpha' : Item("This is the file 'alpha'.\n"),
    'E/beta'  : Item("This is the file 'beta'.\n"),
    'F'       : Item(),
    'lambda'  : Item("This is the file 'lambda'.\n"),
    })
  expected_status = wc.State(B2_path, {
    ''        : Item(status='  '),
    'E'       : Item(status=' M'),
    'E/alpha' : Item(status='  '),
    'E/beta'  : Item(status='  '),
    'F'       : Item(status='  '),
    'lambda'  : Item(status=' M'),
    })
  expected_status.tweak(wc_rev=2)
  expected_skip = wc.State(B2_path, {
    'lambda' : Item(),
    'E'      : Item(),
    })
  svntest.actions.run_and_verify_merge(B2_path, '2', '3', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip)

  expected_output = wc.State(B2_path, {
    'E'       : Item(status='D '),
    'E/alpha' : Item(status='D '),
    'E/beta'  : Item(status='D '),
    'lambda'  : Item(status='D '),
    })
  expected_disk.remove('E/alpha', 'E/beta', 'lambda')
  expected_status.tweak('E', 'E/alpha', 'E/beta', 'lambda', status='D ')
  expected_status.tweak('', status=' M')
  expected_skip.remove('lambda', 'E')

  ### Full-to-dry-run automatic comparison disabled because a) dry-run
  ### doesn't descend into deleted directories, and b) the full merge
  ### notifies deleted directories twice.
  svntest.actions.run_and_verify_merge(B2_path, '2', '3', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, 0, '--force')



#----------------------------------------------------------------------

# Issue 953
def simple_property_merges(sbox):
  "some simple property merges"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a property to a file and a directory
  alpha_path = os.path.join(wc_dir, 'A', 'B', 'E', 'alpha')
  beta_path = os.path.join(wc_dir, 'A', 'B', 'E', 'beta')
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     alpha_path)
  # A binary, non-UTF8 property value
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo\201val',
                                     beta_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     E_path)

  # Commit change as rev 2
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E'       : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    'A/B/E/beta'  : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/E', 'A/B/E/alpha', 'A/B/E/beta',
                        wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Copy B to B2 as rev 3
  B_url = sbox.repo_url + '/A/B'
  B2_url = sbox.repo_url + '/A/B2'

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'copy', '-m', 'copy B to B2',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B_url, B2_url)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Modify a property and add a property for the file and directory
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'mod_foo', alpha_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'bar', 'bar_val', alpha_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'mod\201foo', beta_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'bar', 'bar\201val', beta_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'mod_foo', E_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'bar', 'bar_val', E_path)

  # Commit change as rev 4
  expected_status = svntest.actions.get_virginal_state(wc_dir, 3)
  expected_status.tweak('A/B/E', 'A/B/E/alpha', 'A/B/E/beta',
                        wc_rev=4, status='  ')
  expected_status.add({
    'A/B2'         : Item(status='  ', wc_rev=3),
    'A/B2/E'       : Item(status='  ', wc_rev=3),
    'A/B2/E/alpha' : Item(status='  ', wc_rev=3),
    'A/B2/E/beta'  : Item(status='  ', wc_rev=3),
    'A/B2/F'       : Item(status='  ', wc_rev=3),
    'A/B2/lambda'  : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  pristine_status = expected_status
  pristine_status.tweak(wc_rev=4)

  # Merge B 3:4 into B2
  B2_path = os.path.join(wc_dir, 'A', 'B2')
  expected_output = wc.State(B2_path, {
    'E'        : Item(status=' U'),
    'E/alpha'  : Item(status=' U'),
    'E/beta'   : Item(status=' U'),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-2,4'}),
    'E'        : Item(),
    'E/alpha'  : Item("This is the file 'alpha'.\n"),
    'E/beta'   : Item("This is the file 'beta'.\n"),
    'F'        : Item(),
    'lambda'   : Item("This is the file 'lambda'.\n"),
    })
  expected_disk.tweak('E', 'E/alpha',
                      props={'foo' : 'mod_foo', 'bar' : 'bar_val'})
  expected_disk.tweak('E/beta',
                      props={'foo' : 'mod\201foo', 'bar' : 'bar\201val'})
  expected_status = wc.State(B2_path, {
    ''        : Item(status=' M'),
    'E'       : Item(status=' M'),
    'E/alpha' : Item(status=' M'),
    'E/beta'  : Item(status=' M'),
    'F'       : Item(status='  '),
    'lambda'  : Item(status='  '),
    })
  expected_status.tweak(wc_rev=4)
  expected_skip = wc.State('', { })
  svntest.actions.run_and_verify_merge(B2_path, '3', '4', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)

  # Revert merge
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'revert', '--recursive', wc_dir)
  svntest.actions.run_and_verify_status(wc_dir, pristine_status)

  # Merge B 2:1 into B2
  expected_disk.tweak('', props={SVN_PROP_MERGE_INFO : '/A/B:1'})
  expected_disk.tweak('E', 'E/alpha', 'E/beta', props={})
  svntest.actions.run_and_verify_merge(B2_path, '2', '1', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)

  # Merge B 3:4 into B2 now causes a conflict
  expected_disk.add({
    '' : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,4'}),
    'E/dir_conflicts.prej'
    : Item("Trying to change property 'foo' from 'foo_val' to 'mod_foo',\n"
           + "but it has been locally deleted.\n"),
    'E/alpha.prej'
    : Item("Trying to change property 'foo' from 'foo_val' to 'mod_foo',\n"
           + "but it has been locally deleted.\n"),
    'E/beta.prej'
    : Item("Trying to change property 'foo' from 'foo?\\129val' to"
           + " 'mod?\\129foo',\n"
           + "but it has been locally deleted.\n"),
    })
  expected_disk.tweak('E', 'E/alpha', props={'bar' : 'bar_val'})
  expected_disk.tweak('E/beta', props={'bar' : 'bar\201val'})
  expected_status.tweak('', status=' M')
  expected_status.tweak('E', 'E/alpha', 'E/beta', status=' C')
  expected_output.tweak('E', 'E/alpha', 'E/beta', status=' C')
  svntest.actions.run_and_verify_merge(B2_path, '3', '4', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)

  # issue 1109 : single file property merge.  This test performs a merge
  # that should be a no-op (adding properties that are already present).
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'revert', '--recursive', wc_dir)
  svntest.actions.run_and_verify_status(wc_dir, pristine_status)

  # Copy A at rev 4 to A2 to make revision 5.
  A_url = sbox.repo_url + '/A'
  A2_url = sbox.repo_url + '/A2'
  svntest.actions.run_and_verify_svn(None,
                                     ['\n', 'Committed revision 5.\n'], [],
                                     'copy', '-m', 'copy A to A2',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url, A2_url)

  # Re-root the WC at A2.
  svntest.actions.run_and_verify_svn(None, None, [], 'switch', A2_url, wc_dir)

  # Attempt to re-merge rev 4 of the original A's alpha.  Mergeinfo
  # inherited from A2 (created by its copy from A) allows us to avoid
  # a repeated merge.
  alpha_url = sbox.repo_url + '/A/B/E/alpha'
  alpha_path = os.path.join(wc_dir, 'B', 'E', 'alpha')

  # Cannot use run_and_verify_merge with a file target
  svntest.actions.run_and_verify_svn(None, [], [], 'merge', '-r', '3:4',
                                     alpha_url, alpha_path)

  output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                   'pl', alpha_path)

  saw_foo = 0
  saw_bar = 0
  for line in output:
    if re.match("\\s*foo\\s*$", line):
      saw_foo = 1
    if re.match("\\s*bar\\s*$", line):
      saw_bar = 1

  if not saw_foo or not saw_bar:
    raise svntest.Failure("Expected properties not found")


#----------------------------------------------------------------------
# This is a regression for issue #1176.

def merge_catches_nonexistent_target(sbox):
  "merge should not die if a target file is absent"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Copy G to a new directory, Q.  Create Q/newfile.  Commit a change
  # to Q/newfile.  Now merge that change... into G.  Merge should not
  # error, but should do nothing.

  G_path = os.path.join(wc_dir, 'A', 'D', 'G')
  Q_path = os.path.join(wc_dir, 'A', 'D', 'Q')
  newfile_path = os.path.join(Q_path, 'newfile')
  Q_url = sbox.repo_url + '/A/D/Q'

  # Copy dir A/D/G to A/D/Q
  svntest.actions.run_and_verify_svn(None, None, [], 'cp', G_path, Q_path)

  svntest.main.file_append(newfile_path, 'This is newfile.\n')
  svntest.actions.run_and_verify_svn(None, None, [], 'add', newfile_path)

  # Add newfile to dir G, creating r2.
  expected_output = wc.State(wc_dir, {
    'A/D/Q'          : Item(verb='Adding'),
    'A/D/Q/newfile'  : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/D/Q'         : Item(status='  ', wc_rev=2),
    'A/D/Q/pi'      : Item(status='  ', wc_rev=2),
    'A/D/Q/rho'     : Item(status='  ', wc_rev=2),
    'A/D/Q/tau'     : Item(status='  ', wc_rev=2),
    'A/D/Q/newfile' : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Change newfile, creating r3.
  svntest.main.file_append(newfile_path, 'A change to newfile.\n')
  expected_output = wc.State(wc_dir, {
    'A/D/Q/newfile'  : Item(verb='Sending'),
    })
  expected_status.tweak('A/D/Q/newfile', wc_rev=3)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Merge the change to newfile (from r3) into G, where newfile
  # doesn't exist.
  os.chdir(G_path)
  expected_output = wc.State('', { })
  expected_status = wc.State('', {
    ''     : Item(),
    'pi'   : Item(),
    'rho'  : Item(),
    'tau'  : Item(),
    })
  expected_status.tweak(status='  ', wc_rev=1)
  expected_disk = wc.State('', {
    'pi'   : Item("This is the file 'pi'.\n"),
    'rho'  : Item("This is the file 'rho'.\n"),
    'tau'  : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State('', {
    'newfile' :Item(),
    })
  svntest.actions.run_and_verify_merge('', '2', '3', Q_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip)

#----------------------------------------------------------------------

def merge_tree_deleted_in_target(sbox):
  "merge on deleted directory in target"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Copy B to a new directory, I. Modify B/E/alpha, Remove I/E. Now
  # merge that change... into I.  Merge should not error

  B_path = os.path.join(wc_dir, 'A', 'B')
  I_path = os.path.join(wc_dir, 'A', 'I')
  alpha_path = os.path.join(B_path, 'E', 'alpha')
  B_url = sbox.repo_url + '/A/B'
  I_url = sbox.repo_url + '/A/I'


  # Copy B to I, creating r1.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'cp', B_url, I_url, '-m', 'rev 2')

  # Change some files, creating r2.
  svntest.main.file_append(alpha_path, 'A change to alpha.\n')
  svntest.main.file_append(os.path.join(B_path, 'lambda'), 'change lambda.\n')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'ci', '-m', 'rev 3', B_path)

  # Remove E, creating r3.
  E_url = sbox.repo_url + '/A/I/E'
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'rm', E_url, '-m', 'rev 4')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'up', os.path.join(wc_dir,'A'))

  expected_output = wc.State(I_path, {
    'lambda'  : Item(status='U '),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,3'}),
    'F'       : Item(),
    'lambda'  : Item("This is the file 'lambda'.\nchange lambda.\n"),
    })
  expected_status = wc.State(I_path, {
    ''        : Item(status=' M'),
    'F'       : Item(status='  '),
    'lambda'  : Item(status='M '),
    })
  expected_status.tweak(wc_rev=4)
  expected_skip = wc.State(I_path, {
    'E'       : Item(),
    'E/alpha' : Item(),
    })
  svntest.actions.run_and_verify_merge(I_path, '2', '3', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 0)

#----------------------------------------------------------------------
# Issue #2515

def merge_added_dir_to_deleted_in_target(sbox):
  "merge an added dir on a deleted dir in target"

  sbox.build()
  wc_dir = sbox.wc_dir

  # copy B to a new directory, I.
  # delete F in I.
  # add J to B/F.
  # merge add to I.

  B_url = sbox.repo_url + '/A/B'
  I_url = sbox.repo_url + '/A/I'
  F_url = sbox.repo_url + '/A/I/F'
  J_url = sbox.repo_url + '/A/B/F/J'
  I_path = os.path.join(wc_dir, 'A', 'I')


  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'cp', B_url, I_url, '-m', 'rev 2')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'rm', F_url, '-m', 'rev 3')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'mkdir', '-m', 'rev 4', J_url)

  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                      'up', os.path.join(wc_dir,'A'))

  expected_output = wc.State(I_path, {})
  expected_disk = wc.State('', {
    'E'       : Item(),
    'E/alpha' : Item("This is the file 'alpha'.\n"),
    'E/beta'  : Item("This is the file 'beta'.\n"),
    'lambda'  : Item("This is the file 'lambda'.\n"),
    })
  expected_skip = wc.State(I_path, {
    'F/J' : Item(),
    'F'   : Item(),
    })

  svntest.actions.run_and_verify_merge(I_path, '2', '4', B_url,
                                       expected_output,
                                       expected_disk,
                                       None,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, 0)

#----------------------------------------------------------------------
# This is a regression for issue #1176.

def merge_similar_unrelated_trees(sbox):
  "merging similar trees ancestrally unrelated"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=1249. ##

  sbox.build()
  wc_dir = sbox.wc_dir

  # Simple test.  Make three directories with the same content.
  # Modify some stuff in the second one.  Now merge
  # (firstdir:seconddir->thirddir).

  base1_path = os.path.join(wc_dir, 'base1')
  base2_path = os.path.join(wc_dir, 'base2')
  apply_path = os.path.join(wc_dir, 'apply')

  base1_url = os.path.join(sbox.repo_url + '/base1')
  base2_url = os.path.join(sbox.repo_url + '/base2')

  # Make a tree of stuff ...
  os.mkdir(base1_path)
  svntest.main.file_append(os.path.join(base1_path, 'iota'),
                           "This is the file iota\n")
  os.mkdir(os.path.join(base1_path, 'A'))
  svntest.main.file_append(os.path.join(base1_path, 'A', 'mu'),
                           "This is the file mu\n")
  os.mkdir(os.path.join(base1_path, 'A', 'B'))
  svntest.main.file_append(os.path.join(base1_path, 'A', 'B', 'alpha'),
                           "This is the file alpha\n")
  svntest.main.file_append(os.path.join(base1_path, 'A', 'B', 'beta'),
                           "This is the file beta\n")

  # ... Copy it twice ...
  shutil.copytree(base1_path, base2_path)
  shutil.copytree(base1_path, apply_path)

  # ... Gonna see if merge is naughty or nice!
  svntest.main.file_append(os.path.join(base2_path, 'A', 'mu'),
                           "A new line in mu.\n")
  os.rename(os.path.join(base2_path, 'A', 'B', 'beta'),
            os.path.join(base2_path, 'A', 'B', 'zeta'))

  svntest.actions.run_and_verify_svn(None, None, [],
                                  'add', base1_path, base2_path, apply_path)

  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'ci', '-m', 'rev 2', wc_dir)

  expected_output = wc.State(apply_path, {
    'A/mu'     : Item(status='U '),
    'A/B/zeta' : Item(status='A '),
    'A/B/beta' : Item(status='D '),
    })

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support 'svn merge URL URL path'
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge',
                                     '--ignore-ancestry',
                                     base1_url, base2_url,
                                     shorten_path_kludge(apply_path))
  os.chdir(saved_cwd)

  expected_status = wc.State(apply_path, {
    ''            : Item(status='  '),
    'A'           : Item(status='  '),
    'A/mu'        : Item(status='M '),
    'A/B'         : Item(status='  '),
    'A/B/zeta'    : Item(status='A ', copied='+'),
    'A/B/alpha'   : Item(status='  '),
    'A/B/beta'    : Item(status='D '),
    'iota'        : Item(status='  '),
    })
  expected_status.tweak(wc_rev=2)
  expected_status.tweak('A/B/zeta', wc_rev='-')
  svntest.actions.run_and_verify_status(apply_path, expected_status)

#----------------------------------------------------------------------
def merge_one_file_helper(sbox, arg_flav, record_only = 0):
  "ARG_FLAV is one of 'r' (revision range) or 'c' (single change)."

  if arg_flav not in ('r', 'c', '*'):
    raise svntest.Failure("Unrecognized flavor of merge argument")

  sbox.build()
  wc_dir = sbox.wc_dir

  rho_rel_path = os.path.join('A', 'D', 'G', 'rho')
  rho_path = os.path.join(wc_dir, rho_rel_path)
  G_path = os.path.join(wc_dir, 'A', 'D', 'G')
  rho_url = sbox.repo_url + '/A/D/G/rho'

  # Change rho for revision 2
  svntest.main.file_append(rho_path, 'A new line in rho.\n')

  expected_output = wc.State(wc_dir, { rho_rel_path : Item(verb='Sending'), })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/D/G/rho', wc_rev=2)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Backdate rho to revision 1, so we can merge in the rev 2 changes.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'up', '-r', '1', rho_path)

  # Try one merge with an explicit target; it should succeed.
  # ### Yes, it would be nice to use run_and_verify_merge(), but it
  # appears to be impossible to get the expected_foo trees working
  # right.  I think something is still assuming a directory target.
  if arg_flav == 'r':
    svntest.actions.run_and_verify_svn(None ,
                                       expected_merge_output(2, 'U    ' +
                                                             rho_path + '\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-r', '1:2',
                                       rho_url, rho_path)
  elif arg_flav == 'c':
    svntest.actions.run_and_verify_svn(None ,
                                       expected_merge_output(2, 'U    ' +
                                                             rho_path + '\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-c', '2',
                                       rho_url, rho_path)
  elif arg_flav == '*':
    svntest.actions.run_and_verify_svn(None ,
                                       expected_merge_output(2, 'U    ' +
                                                             rho_path + '\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', rho_url, rho_path)

  expected_status.tweak(wc_rev=1)
  expected_status.tweak('A/D/G/rho', status='MM')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Inspect rho, make sure it's right.
  rho_text = svntest.tree.get_text(rho_path)
  if rho_text != "This is the file 'rho'.\nA new line in rho.\n":
    raise svntest.Failure("Unexpected text in merged '" + rho_path + "'")

  # Restore rho to pristine revision 1, for another merge.
  svntest.actions.run_and_verify_svn(None, None, [], 'revert', rho_path)
  expected_status.tweak('A/D/G/rho', status='  ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Cd into the directory and run merge with no targets.
  # It should still merge into rho.
  saved_cwd = os.getcwd()
  os.chdir(G_path)

  # Cannot use run_and_verify_merge with a file target
  merge_cmd = [ '--username', svntest.main.wc_author,
                '--password', svntest.main.wc_passwd,
                'merge']
  if arg_flav == 'r':
    merge_cmd += ['-r', '1:2']
  elif arg_flav == 'c':
    merge_cmd += ['-c', '2']

  if record_only:
    expected_output = []
    merge_cmd.append('--record-only')
    rho_expected_status = ' M'
  else:
    expected_output = expected_merge_output(2, 'U    rho\n')
    rho_expected_status = 'MM'
  merge_cmd.append(rho_url)

  svntest.actions.run_and_verify_svn(None, expected_output, [], *merge_cmd)

  # Inspect rho, make sure it's right.
  rho_text = svntest.tree.get_text('rho')
  if record_only:
    expected_text = "This is the file 'rho'.\n"
  else:
    expected_text = "This is the file 'rho'.\nA new line in rho.\n"
  if rho_text != expected_text:
    print
    raise svntest.Failure("Unexpected text merged to 'rho' in '" +
                          G_path + "'")
  os.chdir(saved_cwd)

  expected_status.tweak('A/D/G/rho', status=rho_expected_status)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

def merge_one_file_using_r(sbox):
  "merge one file (issue #1150) using the -r option"
  merge_one_file_helper(sbox, 'r')

def merge_one_file_using_c(sbox):
  "merge one file (issue #1150) using the -c option"
  merge_one_file_helper(sbox, 'c')

def merge_one_file_using_implicit_revs(sbox):
  "merge one file without explicit revisions"
  merge_one_file_helper(sbox, '*')

def merge_record_only(sbox):
  "mark a revision range as merged"
  merge_one_file_helper(sbox, 'r', 1)

#----------------------------------------------------------------------
# This is a regression for the enhancement added in issue #785.

def merge_with_implicit_target_helper(sbox, arg_flav):
  "ARG_FLAV is one of 'r' (revision range) or 'c' (single change)."

  if arg_flav not in ('r', 'c', '*'):
    raise svntest.Failure("Unrecognized flavor of merge argument")

  sbox.build()
  wc_dir = sbox.wc_dir

  # Change mu for revision 2
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  orig_mu_text = svntest.tree.get_text(mu_path)
  added_mu_text = ""
  for x in range(2,11):
    added_mu_text = added_mu_text + 'This is line ' + `x` + ' in mu\n'
  svntest.main.file_append(mu_path, added_mu_text)

  # Create expected output tree for initial commit
  expected_output = wc.State(wc_dir, {
    'A/mu' : Item(verb='Sending'),
    })

  # Create expected status tree; all local revisions should be at 1,
  # but mu should be at revision 2.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', wc_rev=2)

  # Initial commit.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Make the "other" working copy
  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  # Try the merge without an explicit target; it should succeed.
  # Can't use run_and_verify_merge cuz it expects a directory argument.
  mu_url = sbox.repo_url + '/A/mu'

  os.chdir(os.path.join(other_wc, 'A'))

  # merge using URL for sourcepath
  if arg_flav == 'r':
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(-2, 'U    mu\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-r', '2:1', mu_url)
  elif arg_flav == 'c':
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(-2, 'U    mu\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-c', '-2', mu_url)
  elif arg_flav == '*':
    # Implicit merge source URL and revision range detection is for
    # forward merges only (e.g. non-reverts).  Undo application of
    # r2 to enable continuation of the test case.
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(-2, 'U    mu\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-c', '-2', mu_url)

  # sanity-check resulting file
  if (svntest.tree.get_text('mu') != orig_mu_text):
    raise svntest.Failure("Unexpected text '%s' in 'mu', expected '%s'" %
                          (svntest.tree.get_text('mu'), orig_mu_text))


  # merge using filename for sourcepath
  # Cannot use run_and_verify_merge with a file target
  if arg_flav == 'r':
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(2, 'G    mu\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-r', '1:2', 'mu')
  elif arg_flav == 'c':
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(2, 'G    mu\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', '-c', '2', 'mu')

  elif arg_flav == '*':
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(2, 'G    mu\n'),
                                       [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'merge', 'mu')

  # sanity-check resulting file
  if (svntest.tree.get_text('mu') != orig_mu_text + added_mu_text):
    raise svntest.Failure("Unexpected text in 'mu'")

def merge_with_implicit_target_using_r(sbox):
  "merging a file w/no explicit target path using -r"
  merge_with_implicit_target_helper(sbox, 'r')

def merge_with_implicit_target_using_c(sbox):
  "merging a file w/no explicit target path using -c"
  merge_with_implicit_target_helper(sbox, 'c')

def merge_with_implicit_target_and_revs(sbox):
  "merging a file w/no explicit target path or revs"
  merge_with_implicit_target_helper(sbox, '*')


#----------------------------------------------------------------------

def merge_with_prev (sbox):
  "merge operations using PREV revision"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Change mu for revision 2
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  orig_mu_text = svntest.tree.get_text(mu_path)
  added_mu_text = ""
  for x in range(2,11):
    added_mu_text = added_mu_text + '\nThis is line ' + `x` + ' in mu'
  added_mu_text += "\n"
  svntest.main.file_append(mu_path, added_mu_text)

  zot_path = os.path.join(wc_dir, 'A', 'zot')

  svntest.main.file_append(zot_path, "bar")
  svntest.main.run_svn(None, 'add', zot_path)

  # Create expected output tree for initial commit
  expected_output = wc.State(wc_dir, {
    'A/mu' : Item(verb='Sending'),
    'A/zot' : Item(verb='Adding'),
    })

  # Create expected status tree; all local revisions should be at 1,
  # but mu should be at revision 2.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', wc_rev=2)
  expected_status.add({'A/zot' : Item(status='  ', wc_rev=2)})

  # Initial commit.
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Make some other working copies
  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  another_wc = sbox.add_wc_path('another')
  svntest.actions.duplicate_dir(wc_dir, another_wc)

  was_cwd = os.getcwd()

  os.chdir(os.path.join(other_wc, 'A'))

  # Try to revert the last change to mu via svn merge
  # Cannot use run_and_verify_merge with a file target
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-2, 'U    mu\n'),
                                     [],
                                     'merge', '-r', 'HEAD:PREV', 'mu')

  # sanity-check resulting file
  if (svntest.tree.get_text('mu') != orig_mu_text):
    raise svntest.Failure("Unexpected text in 'mu'")

  os.chdir(was_cwd)

  other_status = expected_status
  other_status.wc_dir = other_wc
  other_status.tweak('A/mu', status='M ', wc_rev=2)
  other_status.tweak('A/zot', wc_rev=2)
  svntest.actions.run_and_verify_status(other_wc, other_status)

  os.chdir(another_wc)

  # ensure 'A' will be at revision 2
  svntest.actions.run_and_verify_svn(None, None, [], 'up')

  # now try a revert on a directory, and verify that it removed the zot
  # file we had added previously
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'merge', '-r', 'COMMITTED:PREV',
                                     'A', 'A')

  if (svntest.tree.get_text('A/zot') != None):
    raise svntest.Failure("Unexpected text in 'A/zot'")

  os.chdir(was_cwd)

  another_status = expected_status
  another_status.wc_dir = another_wc
  another_status.tweak(wc_rev=2)
  another_status.tweak('A/mu', status='M ')
  another_status.tweak('A/zot', status='D ')
  svntest.actions.run_and_verify_status(another_wc, another_status)

#----------------------------------------------------------------------
# Regression test for issue #1319: 'svn merge' should *not* 'C' when
# merging a change into a binary file, unless it has local mods, or has
# different contents from the left side of the merge.

def merge_binary_file (sbox):
  "merge change into unchanged binary file"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a binary file to the project
  theta_contents = svntest.main.file_read(
    os.path.join(sys.path[0], "theta.bin"), 'rb')
  # Write PNG file data into 'A/theta'.
  theta_path = os.path.join(wc_dir, 'A', 'theta')
  svntest.main.file_write(theta_path, theta_contents, 'wb')

  svntest.main.run_svn(None, 'add', theta_path)

  # Commit the new binary file, creating revision 2.
  expected_output = svntest.wc.State(wc_dir, {
    'A/theta' : Item(verb='Adding  (bin)'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/theta' : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Make the "other" working copy
  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  # Change the binary file in first working copy, commit revision 3.
  svntest.main.file_append(theta_path, "some extra junk")
  expected_output = wc.State(wc_dir, {
    'A/theta' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/theta' : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_other_wc = shorten_path_kludge(other_wc)

  # In second working copy, attempt to 'svn merge -r 2:3'.
  # We should *not* see a conflict during the update, but a 'U'.
  # And after the merge, the status should be 'M'.
  expected_output = wc.State(short_other_wc, {
    'A/theta' : Item(status='U '),
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.add({
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/:3'}),
    'A/theta' : Item(theta_contents + "some extra junk",
                     props={'svn:mime-type' : 'application/octet-stream'}),
    })
  expected_status = svntest.actions.get_virginal_state(short_other_wc, 1)
  expected_status.add({
    ''        : Item(status=' M', wc_rev=1),
    'A/theta' : Item(status='M ', wc_rev=2),
    })
  expected_skip = wc.State('', { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_other_wc, '2', '3',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1)

#----------------------------------------------------------------------
# Regression test for issue #2403: Incorrect 3-way merge of "added"
# binary file which already exists (unmodified) in the WC

def three_way_merge_add_of_existing_binary_file(sbox):
  "3-way merge of 'file add' into existing binary"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create a branch of A, creating revision 2.
  A_url = sbox.repo_url + "/A"
  branch_A_url = sbox.repo_url + "/copy-of-A"
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     "cp",
                                     A_url, branch_A_url,
                                     "-m", "Creating copy-of-A")

  # Add a binary file to the WC.
  theta_contents = svntest.main.file_read(
    os.path.join(sys.path[0], "theta.bin"), 'rb')
  # Write PNG file data into 'A/theta'.
  theta_path = os.path.join(wc_dir, 'A', 'theta')
  svntest.main.file_write(theta_path, theta_contents, 'wb')

  svntest.main.run_svn(None, "add", theta_path)

  # Commit the new binary file to the repos, creating revision 3.
  expected_output = svntest.wc.State(wc_dir, {
    "A/theta" : Item(verb="Adding  (bin)"),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    "A/theta" : Item(status="  ", wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_wc = shorten_path_kludge(wc_dir)

  # In the working copy, attempt to 'svn merge branch_A_url@2 A_url@3 A'.
  # We should *not* see a conflict during the merge, but an 'A'.
  # And after the merge, the status should not report any differences.

  expected_output = wc.State(short_wc, {
    "A"       : Item(status=" G"),
    "A/theta" : Item(status="A "),
    })

  # As greek_state is rooted at / instead of /A (our merge target), we
  # need a sub-tree of it rather than straight copy.
  expected_disk = svntest.main.greek_state.subtree("A")
  expected_disk.add({
    "theta" : Item(theta_contents,
                   props={"svn:mime-type" : "application/octet-stream"}),
    })
  expected_status = svntest.actions.get_virginal_state(short_wc, 1)
  expected_status.add({
    "A/theta" : Item(status="  ", wc_rev=3),
    })
  expected_status.remove("")  # top-level of the WC
  expected_status.remove("iota")
  expected_skip = wc.State("", { })

  os.chdir(svntest.main.work_dir)
  # If we merge into short_wc alone, theta appears at the WC root,
  # which is in the wrong location -- append "/A" to stay on target.
  svntest.actions.run_and_verify_merge2(short_wc + "/A", "2", "3",
                                        branch_A_url, A_url,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        expected_skip,
                                        None, None, None, None, None,
                                        1)

#----------------------------------------------------------------------
# Regression test for Issue #1297:
# A merge that creates a new file followed by an immediate diff
# The diff should succeed.

def merge_in_new_file_and_diff(sbox):
  "diff after merge that creates a new file"

  sbox.build()
  wc_dir = sbox.wc_dir

  trunk_url = sbox.repo_url + '/A/B/E'

  # Create a branch
  svntest.actions.run_and_verify_svn(None, None, [], 'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     trunk_url,
                                     sbox.repo_url + '/branch',
                                     '-m', "Creating the Branch")

  # Update to revision 2.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'update', wc_dir)

  new_file_path = os.path.join(wc_dir, 'A', 'B', 'E', 'newfile')
  svntest.main.file_write(new_file_path, "newfile\n")

  # Add the new file, and commit revision 3.
  svntest.actions.run_and_verify_svn(None, None, [], "add", new_file_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'ci', '-m',
                                     "Changing the trunk.", wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  branch_path = os.path.join(wc_dir, "branch")
  short_branch_path = shorten_path_kludge(branch_path)

  # Merge our addition into the branch.
  expected_output = svntest.wc.State(short_branch_path, {
    'newfile' : Item(status='A '),
    })
  expected_disk = wc.State('', {
    'alpha'   : Item("This is the file 'alpha'.\n"),
    'beta'    : Item("This is the file 'beta'.\n"),
    'newfile' : Item("newfile\n"),
    })
  expected_status = wc.State(short_branch_path, {
    ''        : Item(status=' M', wc_rev=2),
    'alpha'   : Item(status='  ', wc_rev=2),
    'beta'    : Item(status='  ', wc_rev=2),
    'newfile' : Item(status='A ', wc_rev='-', copied='+')
    })
  expected_skip = wc.State('', { })

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_branch_path,
                                       '1', 'HEAD', trunk_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip)

  os.chdir(saved_cwd)

  # Finally, run diff.  This diff produces no output!
  expected_output = [
    "\n",
    "Property changes on: " + branch_path + "\n",
    "___________________________________________________________________\n",
    "Name: " + SVN_PROP_MERGE_INFO + "\n",
    "   Merged /A/B/E:r2-3\n",
    "\n", ]
  svntest.actions.run_and_verify_svn(None, expected_output, [], 'diff',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     branch_path)


#----------------------------------------------------------------------

# Issue #1425:  'svn merge' should skip over any unversioned obstructions.

def merge_skips_obstructions(sbox):
  "merge should skip over unversioned obstructions"

  sbox.build()
  wc_dir = sbox.wc_dir

  C_path = os.path.join(wc_dir, 'A', 'C')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'

  Q_path = os.path.join(F_path, 'Q')
  foo_path = os.path.join(F_path, 'foo')
  bar_path = os.path.join(F_path, 'Q', 'bar')

  svntest.main.run_svn(None, 'mkdir', Q_path)
  svntest.main.file_append(foo_path, "foo")
  svntest.main.file_append(bar_path, "bar")
  svntest.main.run_svn(None, 'add', foo_path, bar_path)

  expected_output = wc.State(wc_dir, {
    'A/B/F/Q'     : Item(verb='Adding'),
    'A/B/F/Q/bar' : Item(verb='Adding'),
    'A/B/F/foo'   : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/Q'     : Item(status='  ', wc_rev=2),
    'A/B/F/Q/bar' : Item(status='  ', wc_rev=2),
    'A/B/F/foo'   : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  pre_merge_status = expected_status

  # Revision 2 now has A/B/F/foo, A/B/F/Q, A/B/F/Q/bar.  Let's merge
  # those 'F' changes into empty dir 'C'.  But first, create an
  # unversioned 'foo' within C, and make sure 'svn merge' doesn't
  # error when the addition of foo is obstructed.

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_C_path = shorten_path_kludge(C_path)

  expected_output = wc.State(short_C_path, {
    'Q'      : Item(status='A '),
    'Q/bar'  : Item(status='A '),
    })
  expected_disk = wc.State('', {
    ''       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:2'}),
    'Q'      : Item(),
    'Q/bar'  : Item("bar"),
    'foo'    : Item("foo"),
    })
  expected_status = wc.State(short_C_path, {
    ''       : Item(status=' M', wc_rev=1),
    'Q'      : Item(status='A ', wc_rev='-', copied='+'),
    'Q/bar'  : Item(status='A ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(short_C_path, {
    'foo' : Item(),
    })
  # Unversioned:
  svntest.main.file_append(os.path.join(C_path, "foo"), "foo")

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_C_path, '1', '2', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 0)

  os.chdir(saved_cwd)

  # Revert the local mods, and this time make "Q" obstructed.  An
  # unversioned file called "Q" will obstruct the adding of the
  # directory of the same name.

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'revert', '-R', wc_dir)
  os.unlink(os.path.join(C_path, "foo"))
  svntest.main.safe_rmtree(os.path.join(C_path, "Q"))
  svntest.main.file_append(os.path.join(C_path, "Q"), "foo") # unversioned
  svntest.actions.run_and_verify_status(wc_dir, pre_merge_status)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we use short_C_path and chdir() below.
  expected_output = wc.State(short_C_path, {
    'foo'  : Item(status='A '),
    })
  expected_disk = wc.State('', {
    ''       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:2'}),
    'Q'      : Item("foo"),
    'foo'    : Item("foo"),
    })
  expected_status = wc.State(short_C_path, {
    ''     : Item(status=' M', wc_rev=1),
    'foo'  : Item(status='A ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(short_C_path, {
    'Q'     : Item(),
    'Q/bar' : Item(),
    })

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_C_path, '1', '2', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 0)

  os.chdir(saved_cwd)

  # Revert the local mods, and commit the deletion of iota and A/D/G. (r3)
  os.unlink(os.path.join(C_path, "foo"))
  svntest.actions.run_and_verify_svn(None, None, [], 'revert', '-R', wc_dir)
  svntest.actions.run_and_verify_status(wc_dir, pre_merge_status)

  iota_path = os.path.join(wc_dir, 'iota')
  G_path = os.path.join(wc_dir, 'A', 'D', 'G')
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', iota_path, G_path)

  expected_output = wc.State(wc_dir, {
    'A/D/G'  : Item(verb='Deleting'),
    'iota'   : Item(verb='Deleting'),
    })
  expected_status = pre_merge_status
  expected_status.remove('iota', 'A/D/G', 'A/D/G/pi', 'A/D/G/rho', 'A/D/G/tau')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Now create unversioned iota and A/D/G, try running a merge -r2:3.
  # The merge process should skip over these targets, since they're
  # unversioned.

  # Note: This merge, and all subsequent merges within this test,
  # skip *all* targets, so no mergeinfo is set.

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_wc_dir = shorten_path_kludge(wc_dir)

  svntest.main.file_append(iota_path, "foo") # unversioned
  os.mkdir(G_path) # unversioned

  expected_output = wc.State(short_wc_dir, { })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('A/D/G/pi', 'A/D/G/rho', 'A/D/G/tau')
  expected_disk.add({
    'A/B/F/Q'      : Item(),
    'A/B/F/Q/bar'  : Item("bar"),
    'A/B/F/foo'    : Item("foo"),
    'iota'         : Item("foo"),
    'A/C/Q'        : Item("foo"),
    })
  expected_skip = wc.State(short_wc_dir, {
    'A/D/G'  : Item(),
    'iota'   : Item(),
    })

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_wc_dir, '2', '3',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status.copy(short_wc_dir),
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 0)

  os.chdir(saved_cwd)

  # Revert the local mods, and commit a change to A/B/lambda (r4), and then
  # commit the deletion of the same file. (r5)
  os.unlink(iota_path)
  svntest.main.safe_rmtree(G_path)
  svntest.actions.run_and_verify_svn(None, None, [], 'revert', '-R', wc_dir)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  lambda_path = os.path.join(wc_dir, 'A', 'B', 'lambda')
  svntest.main.file_append(lambda_path, "more text")
  expected_output = wc.State(wc_dir, {
    'A/B/lambda'  : Item(verb='Sending'),
    })
  expected_status.tweak('A/B/lambda', wc_rev=4)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  svntest.actions.run_and_verify_svn(None, None, [], 'rm', lambda_path)

  expected_output = wc.State(wc_dir, {
    'A/B/lambda'  : Item(verb='Deleting'),
    })
  expected_status.remove('A/B/lambda')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # lambda is gone, so create an unversioned lambda in its place.
  # Then attempt to merge -r3:4, which is a change to lambda.  The merge
  # should simply skip the unversioned file.

  svntest.main.file_append(lambda_path, "foo") # unversioned

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we use short_wc_dir and chdir() below.
  expected_output = wc.State(short_wc_dir, { })
  expected_disk.add({
    'A/B/lambda'      : Item("foo"),
    })
  expected_disk.remove('A/D/G', 'iota')
  expected_skip = wc.State(short_wc_dir, {
    'A/B/lambda'  : Item(),
    })

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_wc_dir, '3', '4',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status.copy(short_wc_dir),
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 0)

  os.chdir(saved_cwd)

  # OK, so let's commit the new lambda (r6), and then delete the
  # working file.  Then re-run the -r3:4 merge, and see how svn deals
  # with a file being under version control, but missing.

  svntest.actions.run_and_verify_svn(None, None, [], 'add', lambda_path)

  expected_output = wc.State(wc_dir, {
    'A/B/lambda'  : Item(verb='Adding'),
    })
  expected_status.add({
    'A/B/lambda'  : Item(wc_rev=6, status='  '),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  os.unlink(lambda_path)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we use short_wc_dir and chdir() below.
  expected_output = wc.State(short_wc_dir, { })
  expected_disk.remove('A/B/lambda')
  expected_status.tweak('A/B/lambda', status='! ')

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_wc_dir, '3', '4',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status.copy(short_wc_dir),
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 0)

#----------------------------------------------------------------------
# At one time, a merge that added items with the same name as missing
# items would attempt to add the items and fail, leaving the working
# copy locked and broken.

def merge_into_missing(sbox):
  "merge into missing must not break working copy"

  sbox.build()
  wc_dir = sbox.wc_dir

  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'
  Q_path = os.path.join(F_path, 'Q')
  foo_path = os.path.join(F_path, 'foo')

  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', Q_path)
  svntest.main.file_append(foo_path, "foo")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', foo_path)

  expected_output = wc.State(wc_dir, {
    'A/B/F/Q'       : Item(verb='Adding'),
    'A/B/F/foo'     : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/Q'       : Item(status='  ', wc_rev=2),
    'A/B/F/foo'     : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  R_path = os.path.join(Q_path, 'R')
  bar_path = os.path.join(R_path, 'bar')
  baz_path = os.path.join(Q_path, 'baz')
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', R_path)
  svntest.main.file_append(bar_path, "bar")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', bar_path)
  svntest.main.file_append(baz_path, "baz")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', baz_path)

  expected_output = wc.State(wc_dir, {
    'A/B/F/Q/R'     : Item(verb='Adding'),
    'A/B/F/Q/R/bar' : Item(verb='Adding'),
    'A/B/F/Q/baz'   : Item(verb='Adding'),
    })
  expected_status.add({
    'A/B/F/Q/R'     : Item(status='  ', wc_rev=3),
    'A/B/F/Q/R/bar' : Item(status='  ', wc_rev=3),
    'A/B/F/Q/baz'   : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  os.unlink(foo_path)
  svntest.main.safe_rmtree(Q_path)

  expected_output = wc.State(F_path, {
    })
  expected_disk = wc.State('', {
    })
  expected_status = wc.State(F_path, {
    ''      : Item(status='  ', wc_rev=1),
    'foo'   : Item(status='! ', wc_rev=2),
    'Q'     : Item(status='! ', wc_rev='?'),
    })
  expected_skip = wc.State(F_path, {
    'Q'   : Item(),
    'foo' : Item(),
    })

  ### Need to real and dry-run separately since real merge notifies Q
  ### twice!
  svntest.actions.run_and_verify_merge(F_path, '1', '2', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, 0, '--dry-run')

  svntest.actions.run_and_verify_merge(F_path, '1', '2', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, 0)

  # This merge fails when it attempts to descend into the missing
  # directory.  That's OK, there is no real need to support merge into
  # an incomplete working copy, so long as when it fails it doesn't
  # break the working copy.
  svntest.main.run_svn('Working copy not locked',
                       'merge', '-r1:3', '--dry-run', F_url, F_path)

  svntest.main.run_svn('Working copy not locked',
                       'merge', '-r1:3', F_url, F_path)

  # Check working copy is not locked.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo' : Item(status='! ', wc_rev=2),
    'A/B/F/Q' : Item(status='! ', wc_rev='?'),
    })
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# A test for issue 1738

def dry_run_adds_file_with_prop(sbox):
  "merge --dry-run adding a new file with props"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Commit a new file which has a property.
  zig_path = os.path.join(wc_dir, 'A', 'B', 'E', 'zig')
  svntest.main.file_append(zig_path, "zig contents")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', zig_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     zig_path)

  expected_output = wc.State(wc_dir, {
    'A/B/E/zig'     : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/E/zig'   : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Do a regular merge of that change into a different dir.
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  E_url = sbox.repo_url + '/A/B/E'

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_F_path = shorten_path_kludge(F_path)

  expected_output = wc.State(short_F_path, {
    'zig'  : Item(status='A '),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:2'}),
    'zig'      : Item("zig contents", {'foo':'foo_val'}),
    })
  expected_skip = wc.State('', { })
  expected_status = None  # status is optional

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_F_path, '1', '2', E_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, # please check props
                                       1) # and do a dry-run also)

#----------------------------------------------------------------------

# Regression test for issue #1673
# Merge a binary file from two URL with a common ancestry

def merge_binary_with_common_ancestry(sbox):
  "merge binary files with common ancestry"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Create the common ancestry path
  I_path = os.path.join(wc_dir, 'I')
  svntest.main.run_svn(None, 'mkdir', I_path)

  # Add a binary file to the common ancestry path
  theta_contents = svntest.main.file_read(
    os.path.join(sys.path[0], "theta.bin"), 'rb')
  theta_I_path = os.path.join(I_path, 'theta')
  svntest.main.file_write(theta_I_path, theta_contents)
  svntest.main.run_svn(None, 'add', theta_I_path)
  svntest.main.run_svn(None, 'propset', 'svn:mime-type',
                       'application/octet-stream', theta_I_path)

  # Commit the ancestry
  expected_output = wc.State(wc_dir, {
    'I'       : Item(verb='Adding'),
    'I/theta' : Item(verb='Adding  (bin)'),
    })

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'I'       : Item(status='  ', wc_rev=2),
    'I/theta' : Item(status='  ', wc_rev=2),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Create the first branch
  J_path = os.path.join(wc_dir, 'J')
  svntest.main.run_svn(None, 'copy', I_path, J_path)

  # Commit the first branch
  expected_output = wc.State(wc_dir, {
    'J' : Item(verb='Adding'),
    })

  expected_status.add({
    'J'       : Item(status='  ', wc_rev=3),
    'J/theta' : Item(status='  ', wc_rev=3),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Create the path where the files will be merged
  K_path = os.path.join(wc_dir, 'K')
  svntest.main.run_svn(None, 'mkdir', K_path)

  # Commit the new path
  expected_output = wc.State(wc_dir, {
    'K' : Item(verb='Adding'),
    })

  expected_status.add({
    'K'       : Item(status='  ', wc_rev=4),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Copy 'I/theta' to 'K/'. This file will be merged later.
  theta_K_path = os.path.join(K_path, 'theta')
  svntest.main.run_svn(None, 'copy', theta_I_path, theta_K_path)

  # Commit the new file
  expected_output = wc.State(wc_dir, {
    'K/theta' : Item(verb='Adding  (bin)'),
    })

  expected_status.add({
    'K/theta' : Item(status='  ', wc_rev=5),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Modify the original ancestry 'I/theta'
  svntest.main.file_append(theta_I_path, "some extra junk")

  # Commit the modification
  expected_output = wc.State(wc_dir, {
    'I/theta' : Item(verb='Sending'),
    })

  expected_status.tweak('I/theta', wc_rev=6)

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Create the second branch from the modified ancestry
  L_path = os.path.join(wc_dir, 'L')
  svntest.main.run_svn(None, 'copy', I_path, L_path)

  # Commit the second branch
  expected_output = wc.State(wc_dir, {
    'L'       : Item(verb='Adding'),
    'L/theta' : Item(verb='Adding  (bin)'),
    })

  expected_status.add({
    'L'       : Item(status='  ', wc_rev=7),
    'L/theta' : Item(status='  ', wc_rev=7),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        wc_dir)

  # Now merge first ('J/') and second ('L/') branches into 'K/'
  saved_cwd = os.getcwd()

  os.chdir(K_path)
  theta_J_url = sbox.repo_url + '/J/theta'
  theta_L_url = sbox.repo_url + '/L/theta'
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(7, 'U    theta\n',
                                                           None, False),
                                     [],
                                     'merge', theta_J_url, theta_L_url)
  os.chdir(saved_cwd)

  expected_status.tweak('K/theta', status='M ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

#----------------------------------------------------------------------
# A test for issue 1905
def merge_funny_chars_on_path(sbox):
  "merge with funny characters (issue #1905)"

  sbox.build()
  wc_dir = sbox.wc_dir

  # In following lists: 'd' stands for directory, 'f' for file
  # targets to be added by recursive add
  add_by_add = [
    ('d', 'dir_10', 'F%lename'),
    ('d', 'dir%20', 'F lename'),
    ('d', 'dir 30', 'Filename'),
    ('d', 'dir 40', None),
    ('f', 'F lename', None),
    ]

  # targets to be added by 'svn mkdir' + add
  add_by_mkdir = [
    ('d', 'dir_11', 'F%lename'),
    ('d', 'dir%21', 'Filename'),
    ('d', 'dir 31', 'F lename'),
    ('d', 'dir 41', None),
    ]

  for target in add_by_add:
    if target[0] == 'd':
      target_dir = os.path.join(wc_dir, 'A', 'B', 'E', target[1])
      os.mkdir(target_dir)
      if target[2]:
        target_path = os.path.join(wc_dir, 'A', 'B', 'E', '%s' % target[1], target[2])
        svntest.main.file_append(target_path, "%s/%s" % (target[1], target[2]))
      svntest.actions.run_and_verify_svn(None, None, [], 'add', target_dir)
    elif target[0] == 'f':
        target_path = os.path.join(wc_dir, 'A', 'B', 'E', '%s' % target[1])
        svntest.main.file_append(target_path, "%s" % target[1])
        svntest.actions.run_and_verify_svn(None, None, [], 'add', target_path)
    else:
      raise svntest.Failure


  for target in add_by_mkdir:
    if target[0] == 'd':
      target_dir = os.path.join(wc_dir, 'A', 'B', 'E', target[1])
      svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', target_dir)
      if target[2]:
        target_path = os.path.join(wc_dir, 'A', 'B', 'E', '%s' % target[1], target[2])
        svntest.main.file_append(target_path, "%s/%s" % (target[1], target[2]))
        svntest.actions.run_and_verify_svn(None, None, [], 'add', target_path)

  expected_output_dic = {}
  expected_status_dic = {}

  for targets in add_by_add,add_by_mkdir:
    for target in targets:
      key = 'A/B/E/%s' % target[1]
      expected_output_dic[key] = Item(verb='Adding')
      expected_status_dic[key] = Item(status='  ', wc_rev=2)

      if target[2]:
        key = 'A/B/E/%s/%s' % (target[1], target[2])
        expected_output_dic[key] = Item(verb='Adding')
        expected_status_dic[key] = Item(status='  ', wc_rev=2)


  expected_output = wc.State(wc_dir, expected_output_dic)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add(expected_status_dic)

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Do a regular merge of that change into a different dir.
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  E_url = sbox.repo_url + '/A/B/E'

  expected_output_dic = {}
  expected_disk_dic = {}

  for targets in add_by_add,add_by_mkdir:
    for target in targets:
      key = '%s' % target[1]
      expected_output_dic[key] = Item(status='A ')
      if target[0] == 'd':
        expected_disk_dic[key] = Item(None, {})
      elif target[0] == 'f':
        expected_disk_dic[key] = Item("%s" % target[1], {})
      else:
        raise svntest.Failure
      if target[2]:
        key = '%s/%s' % (target[1], target[2])
        expected_output_dic[key] = Item(status='A ')
        expected_disk_dic[key] = Item('%s/%s' % (target[1], target[2]), {})


  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_F_path = shorten_path_kludge(F_path)

  expected_output = wc.State(short_F_path, expected_output_dic)

  expected_disk = wc.State('', expected_disk_dic)
  expected_skip = wc.State('', { })
  expected_status = None  # status is optional

  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_F_path, '1', '2', E_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, # don't check props
                                       1) # but do a dry-run
  os.chdir(saved_cwd)

  expected_output_dic = {}

  for targets in add_by_add,add_by_mkdir:
    for target in targets:
      key = '%s' % target[1]
      expected_output_dic[key] = Item(verb='Adding')
      if target[2]:
        key = '%s/%s' % (target[1], target[2])
        expected_output_dic[key] = Item(verb='Adding')

  expected_output = wc.State(F_path, expected_output_dic)
  expected_output.add({
    '' : Item(verb='Sending'),
    })

  svntest.actions.run_and_verify_commit(F_path,
                                        expected_output,
                                        None,
                                        None, None, None, None, None,
                                        wc_dir)

#-----------------------------------------------------------------------
# Regression test for issue #2064

def merge_keyword_expansions(sbox):
  "merge changes to keyword expansion property"

  sbox.build()

  wcpath = sbox.wc_dir
  tpath = os.path.join(wcpath, "t")
  bpath = os.path.join(wcpath, "b")
  t_fpath = os.path.join(tpath, 'f')
  b_fpath = os.path.join(bpath, 'f')

  os.mkdir(tpath)
  svntest.main.run_svn(None, "add", tpath)
  # Commit r2.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     "ci", "-m", "r2", wcpath)

  # Copy t to b.
  svntest.main.run_svn(None, "cp", tpath, bpath)
  # Commit r3
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     "ci", "-m", "r3", wcpath)

  # Add a file to t.
  svntest.main.file_append(t_fpath, "$Revision$")
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'add', t_fpath)
  # Ask for keyword expansion in the file.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'svn:keywords', 'Revision',
                                     t_fpath)
  # Commit r4
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'ci', '-m', 'r4', wcpath)

  # Update the wc before the merge.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'update', wcpath)

  expected_status = svntest.actions.get_virginal_state(wcpath, 4)
  expected_status.add({
    't'    : Item(status='  ', wc_rev=4),
    't/f'  : Item(status='  ', wc_rev=4),
    'b'    : Item(status='  ', wc_rev=4),
  })
  svntest.actions.run_and_verify_status(wcpath, expected_status)

  # Do the merge.

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_bpath = shorten_path_kludge(bpath)

  expected_output = wc.State(short_bpath, {
    'f'  : Item(status='A '),
    })
  expected_disk = wc.State('', {
    'f'      : Item("$Revision: 4 $"),
    })
  expected_status = wc.State(short_bpath, {
    ''       : Item(status=' M', wc_rev=4),
    'f'      : Item(status='A ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(short_bpath, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_bpath, '2', 'HEAD',
                                       sbox.repo_url + '/t',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip)

#----------------------------------------------------------------------
def merge_prop_change_to_deleted_target(sbox):
  "merge prop change into deleted target"
  # For issue #2132.
  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a property to alpha.
  alpha_path = os.path.join(wc_dir, 'A', 'B', 'E', 'alpha')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     alpha_path)

  # Commit the property add as r2.
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E/alpha' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/E/alpha', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'up', wc_dir)

  # Remove alpha entirely.
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', alpha_path)
  expected_output = wc.State(wc_dir, {
    'A/B/E/alpha'  : Item(verb='Deleting'),
    })
  expected_status.tweak(wc_rev=2)
  expected_status.remove('A/B/E/alpha')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        alpha_path)

  # Try merging the original propset, which applies to a target that
  # no longer exists.  The bug would only reproduce when run from
  # inside the wc, so we cd in there.
  os.chdir(wc_dir)
  svntest.actions.run_and_verify_svn("Merge errored unexpectedly",
                                     svntest.verify.AnyOutput, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge', '-r1:2', '.')


def setup_dir_replace(sbox):
  """Setup the working copy for directory replace tests, creating
  directory 'A/B/F/foo' with files 'new file' and 'new file2' within
  it (r2), and merging 'foo' onto 'C' (r3), then deleting 'A/B/F/foo'
  (r4)."""

  sbox.build()
  wc_dir = sbox.wc_dir

  C_path = os.path.join(wc_dir, 'A', 'C')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'

  foo_path = os.path.join(F_path, 'foo')
  new_file = os.path.join(foo_path, "new file")
  new_file2 = os.path.join(foo_path, "new file 2")

  # Make directory foo in F, and add some files within it.
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', foo_path)
  svntest.main.file_append(new_file, "Initial text in new file.\n")
  svntest.main.file_append(new_file2, "Initial text in new file 2.\n")
  svntest.main.run_svn(None, "add", new_file)
  svntest.main.run_svn(None, "add", new_file2)

  # Commit all the new content, creating r2.
  expected_output = wc.State(wc_dir, {
    'A/B/F/foo'            : Item(verb='Adding'),
    'A/B/F/foo/new file'   : Item(verb='Adding'),
    'A/B/F/foo/new file 2' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'             : Item(status='  ', wc_rev=2),
    'A/B/F/foo/new file'    : Item(status='  ', wc_rev=2),
    'A/B/F/foo/new file 2'  : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Merge foo onto C
  expected_output = wc.State(C_path, {
    'foo' : Item(status='A '),
    'foo/new file'   : Item(status='A '),
    'foo/new file 2' : Item(status='A '),
    })
  expected_disk = wc.State('', {
    ''               : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:2'}),
    'foo' : Item(),
    'foo/new file'   : Item("Initial text in new file.\n"),
    'foo/new file 2' : Item("Initial text in new file 2.\n"),
    })
  expected_status = wc.State(C_path, {
    ''    : Item(status=' M', wc_rev=1),
    'foo' : Item(status='A ', wc_rev='-', copied='+'),
    'foo/new file'   : Item(status='A ', wc_rev='-', copied='+'),
    'foo/new file 2' : Item(status='A ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(C_path, { })
  svntest.actions.run_and_verify_merge(C_path, '1', '2', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  # Commit merge of foo onto C, creating r3.
  expected_output = svntest.wc.State(wc_dir, {
    'A/C'        : Item(verb='Sending'),
    'A/C/foo'    : Item(verb='Adding'),
    'A/C/foo/new file'      : Item(verb='Adding'),
    'A/C/foo/new file 2'    : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'  : Item(status='  ', wc_rev=2),
    'A/C'        : Item(status='  ', wc_rev=3),
    'A/B/F/foo/new file'      : Item(status='  ', wc_rev=2),
    'A/B/F/foo/new file 2'    : Item(status='  ', wc_rev=2),
    'A/C/foo'    : Item(status='  ', wc_rev=3),
    'A/C/foo/new file'      : Item(status='  ', wc_rev=3),
    'A/C/foo/new file 2'    : Item(status='  ', wc_rev=3),

    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Delete foo on F, creating r4.
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', foo_path)
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/F/foo'   : Item(verb='Deleting'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/C'         : Item(status='  ', wc_rev=3),
    'A/C/foo'     : Item(status='  ', wc_rev=3),
    'A/C/foo/new file'      : Item(status='  ', wc_rev=3),
    'A/C/foo/new file 2'    : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

#----------------------------------------------------------------------
# A merge that replaces a directory
# Tests for Issue #2144 and Issue #2607

def merge_dir_replace(sbox):
  "merge a replacement of a directory"

  setup_dir_replace(sbox)
  wc_dir = sbox.wc_dir

  C_path = os.path.join(wc_dir, 'A', 'C')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'
  foo_path = os.path.join(F_path, 'foo')
  new_file2 = os.path.join(foo_path, "new file 2")

  # Recreate foo in F and add a new folder and two files
  bar_path = os.path.join(foo_path, 'bar')
  foo_file = os.path.join(foo_path, "file foo")
  new_file3 = os.path.join(bar_path, "new file 3")

  # Make a couple of directories, and add some files within them.
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', foo_path)
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', bar_path)
  svntest.main.file_append(new_file3, "Initial text in new file 3.\n")
  svntest.main.run_svn(None, "add", new_file3)
  svntest.main.file_append(foo_file, "Initial text in file foo.\n")
  svntest.main.run_svn(None, "add", foo_file)

  # Commit the new content, creating r5.
  expected_output = wc.State(wc_dir, {
    'A/B/F/foo'                : Item(verb='Adding'),
    'A/B/F/foo/file foo'       : Item(verb='Adding'),
    'A/B/F/foo/bar'            : Item(verb='Adding'),
    'A/B/F/foo/bar/new file 3' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'             : Item(status='  ', wc_rev=5),
    'A/B/F/foo/file foo'    : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar'         : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar/new file 3'  : Item(status='  ', wc_rev=5),
    'A/C'                   : Item(status='  ', wc_rev=3),
    'A/C/foo'               : Item(status='  ', wc_rev=3),
    'A/C/foo/new file'      : Item(status='  ', wc_rev=3),
    'A/C/foo/new file 2'    : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  # Merge replacement of foo onto C
  expected_output = wc.State(C_path, {
    'foo' : Item(status='D '),
    'foo' : Item(status='A '),
    'foo/new file 2' : Item(status='D '),
    'foo/file foo'   : Item(status='A '),
    'foo/bar'        : Item(status='A '),
    'foo/bar/new file 3' : Item(status='A '),
    'foo/new file'   : Item(status='D '),
    })
  expected_disk = wc.State('', {
    ''    : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:2-5'}),
    'foo' : Item(),
    'foo/file foo'       : Item("Initial text in file foo.\n"),
    'foo/bar' : Item(),
    'foo/bar/new file 3' : Item("Initial text in new file 3.\n"),
    })
  expected_status = wc.State(C_path, {
    ''    : Item(status=' M', wc_rev=3),
    'foo' : Item(status='R ', wc_rev='-', copied='+'),
    'foo/new file 2' : Item(status='D ', wc_rev='-', copied='+'),
    'foo/file foo'       : Item(status='A ', wc_rev='-', copied='+'),
    'foo/bar'            : Item(status='A ', wc_rev='-', copied='+'),
    'foo/bar/new file 3' : Item(status='A ', wc_rev='-', copied='+'),
    'foo/new file'   : Item(status='D ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(C_path, { })
  svntest.actions.run_and_verify_merge(C_path, '2', '5', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1,
                                       0) # don't do a dry-run the output differs

  # Commit merge of foo onto C
  expected_output = svntest.wc.State(wc_dir, {
    'A/C'                    : Item(verb='Sending'),
    'A/C/foo'    : Item(verb='Replacing'),
    'A/C/foo/file foo'       : Item(verb='Adding'),
    'A/C/foo/bar'            : Item(verb='Adding'),
    'A/C/foo/bar/new file 3' : Item(verb='Adding'),
    'A/C/foo/new file'       : Item(verb='Deleting'),
    'A/C/foo/new file 2'     : Item(verb='Deleting'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'             : Item(status='  ', wc_rev=5),
    'A/B/F/foo/file foo'    : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar'         : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar/new file 3'  : Item(status='  ', wc_rev=5),
    'A/C'                       : Item(status='  ', wc_rev=6),
    'A/C/foo'                   : Item(status='  ', wc_rev=6),
    'A/C/foo/file foo'          : Item(status='  ', wc_rev=6),
    'A/C/foo/bar'               : Item(status='  ', wc_rev=6),
    'A/C/foo/bar/new file 3'    : Item(status='  ', wc_rev=6),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

#----------------------------------------------------------------------
# A merge that replaces a directory and one of its children
# Tests for Issue #2690

def merge_dir_and_file_replace(sbox):
  "replace both dir and one of its children"

  setup_dir_replace(sbox)
  wc_dir = sbox.wc_dir

  C_path = os.path.join(wc_dir, 'A', 'C')
  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'
  foo_path = os.path.join(F_path, 'foo')
  new_file2 = os.path.join(foo_path, "new file 2")

  # Recreate foo and 'new file 2' in F and add a new folder with a file
  bar_path = os.path.join(foo_path, 'bar')
  new_file3 = os.path.join(bar_path, "new file 3")
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', foo_path)
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', bar_path)
  svntest.main.file_append(new_file3, "Initial text in new file 3.\n")
  svntest.main.run_svn(None, "add", new_file3)
  svntest.main.file_append(new_file2, "New text in new file 2.\n")
  svntest.main.run_svn(None, "add", new_file2)

  expected_output = wc.State(wc_dir, {
    'A/B/F/foo' : Item(verb='Adding'),
    'A/B/F/foo/new file 2'     : Item(verb='Adding'),
    'A/B/F/foo/bar'            : Item(verb='Adding'),
    'A/B/F/foo/bar/new file 3' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'             : Item(status='  ', wc_rev=5),
    'A/B/F/foo/new file 2'  : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar'         : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar/new file 3'  : Item(status='  ', wc_rev=5),
    'A/C/foo'               : Item(status='  ', wc_rev=3),
    'A/C/foo/new file'      : Item(status='  ', wc_rev=3),
    'A/C/foo/new file 2'    : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  # Merge replacement of foo onto C
  expected_output = wc.State(C_path, {
    'foo' : Item(status='D '),
    'foo' : Item(status='A '),
    'foo/new file 2' : Item(status='D '),
    'foo/new file 2' : Item(status='A '),
    'foo/bar'        : Item(status='A '),
    'foo/bar/new file 3' : Item(status='A '),
    'foo/new file'   : Item(status='D '),
    })
  expected_disk = wc.State('', {
    ''    : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:2-5'}),
    'foo' : Item(),
    'foo/new file 2' : Item("New text in new file 2.\n"),
    'foo/bar' : Item(),
    'foo/bar/new file 3' : Item("Initial text in new file 3.\n"),
    })
  expected_status = wc.State(C_path, {
    ''    : Item(status='  ', wc_rev=1),
    'foo' : Item(status='R ', wc_rev='-', copied='+'),
    'foo/new file 2'     : Item(status='R ', wc_rev='-', copied='+'),
    'foo/bar'            : Item(status='A ', wc_rev='-', copied='+'),
    'foo/bar/new file 3' : Item(status='A ', wc_rev='-', copied='+'),
    'foo/new file'       : Item(status='D ', wc_rev='-', copied='+'),
    })
  expected_skip = wc.State(C_path, { })
  svntest.actions.run_and_verify_merge(C_path, '2', '5', F_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1,
                                       0) # don't do a dry-run the output differs

  # Commit merge of foo onto C
  expected_output = svntest.wc.State(wc_dir, {
    'A/C/foo'                : Item(verb='Replacing'),
    'A/C/foo/new file 2'     : Item(verb='Replacing'),
    'A/C/foo/new file'       : Item(verb='Deleting'),
    'A/C/foo/bar'            : Item(verb='Adding'),
    'A/C/foo/bar/new file 3' : Item(verb='Adding'),

    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'                : Item(status='  ', wc_rev=5),
    'A/B/F/foo/new file 2'     : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar'            : Item(status='  ', wc_rev=5),
    'A/B/F/foo/bar/new file 3' : Item(status='  ', wc_rev=5),
    'A/C/foo'                  : Item(status='  ', wc_rev=6),
    'A/C/foo/new file 2'       : Item(status='  ', wc_rev=6),
    'A/C/foo/bar'              : Item(status='  ', wc_rev=6),
    'A/C/foo/bar/new file 3'   : Item(status='  ', wc_rev=6),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

#----------------------------------------------------------------------
def merge_file_with_space_in_its_name(sbox):
  "merge a file whose name contains a space"
  # For issue #2144
  sbox.build()
  wc_dir = sbox.wc_dir
  new_file = os.path.join(wc_dir, "new file")

  # Make r2.
  svntest.main.file_append(new_file, "Initial text in the file.\n")
  svntest.main.run_svn(None, "add", new_file)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     "ci", "-m", "r2", wc_dir)

  # Make r3.
  svntest.main.file_append(new_file, "Next line of text in the file.\n")
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     "ci", "-m", "r3", wc_dir)

  # Try to reverse merge.
  #
  # The reproduction recipe requires that no explicit merge target be
  # passed, so we run merge from inside the wc dir where the target
  # file (i.e., the URL basename) lives.
  os.chdir(wc_dir)
  target_url = sbox.repo_url + '/new%20file'
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     "merge", "-r3:2", target_url)

#----------------------------------------------------------------------
# A merge between two branches using no revision number with the dir being
# created already existing as an unversioned directory.
# Tests for Issue #2222

def merge_dir_branches(sbox):
  "merge between branches (Issue #2222)"

  sbox.build()
  wc_dir = sbox.wc_dir

  F_path = os.path.join(wc_dir, 'A', 'B', 'F')
  F_url = sbox.repo_url + '/A/B/F'
  C_url = sbox.repo_url + '/A/C'

  # Create foo in F
  foo_path = os.path.join(F_path, 'foo')
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', foo_path)

  expected_output = wc.State(wc_dir, {
    'A/B/F/foo' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/F/foo'    : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Create an unversioned foo
  foo_path = os.path.join(wc_dir, 'foo')
  os.mkdir(foo_path)

  # Merge from C to F onto the wc_dir
  # We can't use run_and_verify_merge because it doesn't support this
  # syntax of the merge command.
  ### TODO: We can use run_and_verify_merge2() here now.
  expected_output = expected_merge_output(None, "A    " + foo_path + "\n")
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'merge', C_url, F_url, wc_dir)

  # Run info to check the copied rev to make sure it's right
  expected_output = ["Path: " + foo_path + "\n",
                     "URL: " + sbox.repo_url + "/foo\n",
                     "Repository Root: " + sbox.repo_url + "\n",
                     "Revision: 2\n",
                     "Node Kind: directory\n",
                     "Schedule: add\n",
                     "Copied From URL: " + F_url + "/foo\n",
                     "Copied From Rev: 2\n", "\n"]
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'info', foo_path)


#----------------------------------------------------------------------

def safe_property_merge(sbox):
  "property merges don't overwrite existing prop-mods"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a property to two files and a directory, commit as r2.
  alpha_path = os.path.join(wc_dir, 'A', 'B', 'E', 'alpha')
  beta_path = os.path.join(wc_dir, 'A', 'B', 'E', 'beta')
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     alpha_path, beta_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     E_path)

  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E'       : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    'A/B/E/beta'  : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/E', 'A/B/E/alpha', 'A/B/E/beta',
                        wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Copy B to B2 as rev 3  (making a branch)
  B_url = sbox.repo_url + '/A/B'
  B2_url = sbox.repo_url + '/A/B2'

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'copy', '-m', 'copy B to B2',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B_url, B2_url)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Change the properties underneath B again, and commit as r4
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val2',
                                     alpha_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propdel', 'foo',
                                     beta_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val2',
                                     E_path)
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E'       : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    'A/B/E/beta'  : Item(verb='Sending'),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, None,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Make local propchanges to E, alpha and beta in the branch.
  alpha_path2 = os.path.join(wc_dir, 'A', 'B2', 'E', 'alpha')
  beta_path2 = os.path.join(wc_dir, 'A', 'B2', 'E', 'beta')
  E_path2 = os.path.join(wc_dir, 'A', 'B2', 'E')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'branchval',
                                     alpha_path2, beta_path2)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'branchval',
                                     E_path2)

  # Now merge the recent B change to the branch.  Because we already
  # have local propmods, we should get property conflicts.
  B2_path = os.path.join(wc_dir, 'A', 'B2')

  expected_output = wc.State(B2_path, {
    'E'        : Item(status=' C'),
    'E/alpha'  : Item(status=' C'),
    'E/beta'   : Item(status=' C'),
    })

  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : "/A/B:1-2,4"}),
    'E'        : Item(),
    'E/alpha'  : Item("This is the file 'alpha'.\n"),
    'E/beta'   : Item("This is the file 'beta'.\n"),
    'F'        : Item(),
    'lambda'   : Item("This is the file 'lambda'.\n"),
    })
  expected_disk.tweak('E', 'E/alpha', 'E/beta',
                      props={'foo' : 'branchval'}) # local mods still present

  expected_status = wc.State(B2_path, {
    ''        : Item(status=' M'),
    'E'       : Item(status=' C'),
    'E/alpha' : Item(status=' C'),
    'E/beta'  : Item(status=' C'),
    'F'       : Item(status='  '),
    'lambda'  : Item(status='  '),
    })
  expected_status.tweak(wc_rev=4)

  expected_skip = wc.State('', { })

  # should have 3 'prej' files left behind, describing prop conflicts:
  extra_files = ['alpha.*\.prej', 'beta.*\.prej', 'dir_conflicts.*\.prej']

  svntest.actions.run_and_verify_merge(B2_path, '3', '4', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, # expected error string
                                       svntest.tree.detect_conflict_files,
                                       extra_files,
                                       None, None, # no B singleton handler
                                       1, # check props
                                       0) # dry_run

#----------------------------------------------------------------------

# Test for issue 2035, whereby 'svn merge' wouldn't always mark
# property conflicts when it should.

def property_merge_from_branch(sbox):
  "property merge conflict even without local mods"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a property to a file and a directory, commit as r2.
  alpha_path = os.path.join(wc_dir, 'A', 'B', 'E', 'alpha')
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     alpha_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     E_path)

  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E'       : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/E', 'A/B/E/alpha', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Copy B to B2 as rev 3  (making a branch)
  B_url = sbox.repo_url + '/A/B'
  B2_url = sbox.repo_url + '/A/B2'

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'copy', '-m', 'copy B to B2',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B_url, B2_url)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Change the properties underneath B again, and commit as r4
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val2',
                                     alpha_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val2',
                                     E_path)
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E'       : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, None,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Make different propchanges changes to the B2 branch and commit as r5.
  alpha_path2 = os.path.join(wc_dir, 'A', 'B2', 'E', 'alpha')
  E_path2 = os.path.join(wc_dir, 'A', 'B2', 'E')

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'branchval',
                                     alpha_path2)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'branchval',
                                     E_path2)
  expected_output = svntest.wc.State(wc_dir, {
    'A/B2/E'       : Item(verb='Sending'),
    'A/B2/E/alpha' : Item(verb='Sending'),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, None,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Now merge the recent B change to the branch.  There are no local
  # mods anywhere, but we should still get property conflicts anyway!
  B2_path = os.path.join(wc_dir, 'A', 'B2')

  expected_output = wc.State(B2_path, {
    'E'        : Item(status=' C'),
    'E/alpha'  : Item(status=' C'),
    })

  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-2,4'}),
    'E'        : Item(),
    'E/alpha'  : Item("This is the file 'alpha'.\n"),
    'E/beta'   : Item("This is the file 'beta'.\n"),
    'F'        : Item(),
    'lambda'   : Item("This is the file 'lambda'.\n"),
    })
  expected_disk.tweak('E', 'E/alpha',
                      props={'foo' : 'branchval'})

  expected_status = wc.State(B2_path, {
    ''        : Item(status=' M'),
    'E'       : Item(status=' C'),
    'E/alpha' : Item(status=' C'),
    'E/beta'  : Item(status='  '),
    'F'       : Item(status='  '),
    'lambda'  : Item(status='  '),
    })
  expected_status.tweak(wc_rev=5)

  expected_skip = wc.State('', { })

  # should have 2 'prej' files left behind, describing prop conflicts:
  extra_files = ['alpha.*\.prej', 'dir_conflicts.*\.prej']

  svntest.actions.run_and_verify_merge(B2_path, '3', '4', B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, # expected error string
                                       svntest.tree.detect_conflict_files,
                                       extra_files,
                                       None, None, # no B singleton handler
                                       1, # check props
                                       0) # dry_run

#----------------------------------------------------------------------

# Another test for issue 2035, whereby sometimes 'svn merge' marked
# property conflicts when it shouldn't!

def property_merge_undo_redo(sbox):
  "undo, then redo a property merge"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a property to a file, commit as r2.
  alpha_path = os.path.join(wc_dir, 'A', 'B', 'E', 'alpha')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo_val',
                                     alpha_path)

  expected_output = svntest.wc.State(wc_dir, {
    'A/B/E/alpha' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/E/alpha', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output, expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Use 'svn merge' to undo the commit.  ('svn merge -r2:1')
  # Result should be a single local-prop-mod.
  expected_output = wc.State(wc_dir, {'A/B/E/alpha'  : Item(status=' U'), })

  expected_disk = svntest.main.greek_state.copy()

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak('A/B/E/alpha', status=' M')

  expected_skip = wc.State('', { })

  svntest.actions.run_and_verify_merge(wc_dir, '2', '1',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, # expected error string
                                       None, None, # no A singleton handler
                                       None, None, # no B singleton handler
                                       1, # check props
                                       0) # dry_run

  # Change mind, re-apply the change ('svn merge -r1:2').
  # This should merge cleanly into existing prop-mod, status shows nothing.
  expected_output = wc.State(wc_dir, {'A/B/E/alpha'  : Item(status=' C'), })

  expected_disk = svntest.main.greek_state.copy()
  expected_disk.add({'' : Item(props={SVN_PROP_MERGE_INFO : '/:2'}), })
  expected_disk.add({'A/B/E/alpha.prej'
     : Item("Trying to create property 'foo' with value 'foo_val',\n"
            + "but it has been locally deleted.\n")})

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak('', status=' M')
  expected_status.tweak('A/B/E/alpha', status=' C')

  expected_skip = wc.State('', { })

  svntest.actions.run_and_verify_merge(wc_dir, '1', '2',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, # expected error string
                                       None, None, # no A singleton handler
                                       None, None, # no B singleton handler
                                       1, # check props
                                       0) # dry_run



#----------------------------------------------------------------------
def cherry_pick_text_conflict(sbox):
  "cherry-pick a dependent change, get conflict"

  sbox.build()
  wc_dir = sbox.wc_dir

  A_path = os.path.join(wc_dir, 'A')
  A_url = sbox.repo_url + '/A'
  mu_path = os.path.join(A_path, 'mu')
  branch_A_url = sbox.repo_url + '/copy-of-A'
  branch_mu_path = os.path.join(wc_dir, 'copy-of-A', 'mu')

  # Create a branch of A.
  svntest.actions.run_and_verify_svn(None, None, [], 'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url, branch_A_url,
                                     '-m', "Creating copy-of-A")

  # Update to get the branch.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'update', wc_dir)

  # Change mu's text on the branch, producing r3 through r6.
  for rev in range(3, 7):
    svntest.main.file_append(branch_mu_path, ("r%d\n" % rev) * 3)
    svntest.actions.run_and_verify_svn(None, None, [],
                                       '--username', svntest.main.wc_author,
                                       '--password', svntest.main.wc_passwd,
                                       'ci', '-m',
                                       'Add lines to mu in r%d.' % rev, wc_dir)

  # Mark r5 as merged into trunk, to create disparate revision ranges
  # which need to be merged.
  svntest.actions.run_and_verify_svn(None, [], [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge', '-c5', '--record-only',
                                     branch_A_url, A_path)


  # Try to merge r4:6 into trunk, without r3.  It should fail.
  expected_output = wc.State(A_path, {
    'mu'       : Item(status='C '),
    })
  expected_disk = wc.State('', {
    'mu'        : Item("This is the file 'mu'.\n"
                       + make_conflict_marker_text("r3\n" * 3, "r4\n" * 3, 4)),
    'B'         : Item(),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/E'       : Item(),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("This is the file 'beta'.\n"),
    'B/F'       : Item(),
    'C'         : Item(),
    'D'         : Item(),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D/H'       : Item(),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
    'D/H/psi'   : Item("This is the file 'psi'.\n"),
    'D/H/omega' : Item("This is the file 'omega'.\n"),
    'D/G'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("This is the file 'rho'.\n"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    })
  expected_status = wc.State(A_path, {
    ''          : Item(status=' M'),
    'mu'        : Item(status='C '),
    'B'         : Item(status='  '),
    'B/lambda'  : Item(status='  '),
    'B/E'       : Item(status='  '),
    'B/E/alpha' : Item(status='  '),
    'B/E/beta'  : Item(status='  '),
    'B/F'       : Item(status='  '),
    'C'         : Item(status='  '),
    'D'         : Item(status='  '),
    'D/gamma'   : Item(status='  '),
    'D/H'       : Item(status='  '),
    'D/H/chi'   : Item(status='  '),
    'D/H/psi'   : Item(status='  '),
    'D/H/omega' : Item(status='  '),
    'D/G'       : Item(status='  '),
    'D/G/pi'    : Item(status='  '),
    'D/G/rho'   : Item(status='  '),
    'D/G/tau'   : Item(status='  '),
    })
  expected_status.tweak(wc_rev=2)
  expected_skip = wc.State('', { })
  expected_error = "conflicts were produced while merging r3:4"
  svntest.actions.run_and_verify_merge(A_path, '3', '6', branch_A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       expected_error,
                                       svntest.tree.detect_conflict_files,
                                       ["mu\.working",
                                        "mu\.merge-right\.r4",
                                        "mu\.merge-left\.r3"],
                                       None, None, # no singleton handler
                                       0, # don't check props
                                       0) # not a dry_run



# Test for issue 2135
def merge_file_replace(sbox):
  "merge a replacement of a file"

  sbox.build()
  wc_dir = sbox.wc_dir

  # File scheduled for deletion
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', rho_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/D/G/rho', status='D ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(verb='Deleting'),
    })

  expected_status.remove('A/D/G/rho')

  # Commit rev 2
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)
  # Create and add a new file.
  svntest.main.file_write(rho_path, "new rho\n")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', rho_path)

  # Commit revsion 3
  expected_status.add({
    'A/D/G/rho' : Item(status='A ', wc_rev='0')
    })
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(verb='Adding'),
    })

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        None,
                                        None, None, None, None, None,
                                        wc_dir)

  # Update working copy
  expected_output = svntest.wc.State(wc_dir, {})
  expected_disk   = svntest.main.greek_state.copy()
  expected_disk.tweak('A/D/G/rho', contents='new rho\n' )
  expected_status.tweak(wc_rev='3')
  expected_status.tweak('A/D/G/rho', status='  ')

  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status)

  # merge changes from r3:1
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(status='A ')
    })
  expected_status.tweak('A/D/G/rho', status='R ', copied='+', wc_rev='-')
  expected_skip = wc.State(wc_dir, { })
  expected_disk.tweak('A/D/G/rho', contents="This is the file 'rho'.\n")
  svntest.actions.run_and_verify_merge(wc_dir, '3', '1',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip)

  # Now commit merged wc
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(verb='Replacing'),
    })
  expected_status.tweak('A/D/G/rho', status='  ', copied=None, wc_rev='4')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)
# Test for issue 2522
# Same as merge_file_replace, but without update before merge.
def merge_file_replace_to_mixed_rev_wc(sbox):
  "merge a replacement of a file to mixed rev wc"

  sbox.build()
  wc_dir = sbox.wc_dir

  # File scheduled for deletion
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', rho_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/D/G/rho', status='D ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(verb='Deleting'),
    })

  expected_status.remove('A/D/G/rho')

  # Commit rev 2
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Update working copy
  expected_disk   = svntest.main.greek_state.copy()
  expected_disk.remove('A/D/G/rho' )
  expected_output = svntest.wc.State(wc_dir, {})
  expected_status.tweak(wc_rev='2')

  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status)

  # Create and add a new file.
  svntest.main.file_write(rho_path, "new rho\n")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', rho_path)

  # Commit revsion 3
  expected_status.add({
    'A/D/G/rho' : Item(status='A ', wc_rev='0')
    })
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(verb='Adding'),
    })

  expected_disk.add({'A/D/G/rho' : Item(contents='new rho\n')} )
  expected_status.tweak(wc_rev='2')
  expected_status.tweak('A/D/G/rho', status='  ', wc_rev='3')

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)


  # merge changes from r3:1
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(status='A ')
    })
  expected_status.tweak('A/D/G/rho', status='R ', copied='+', wc_rev='-')
  expected_skip = wc.State(wc_dir, { })
  expected_disk.tweak('A/D/G/rho', contents="This is the file 'rho'.\n")
  svntest.actions.run_and_verify_merge(wc_dir, '3', '1',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip)

  # At this point WC is broken, because file rho has invalid revision
  # Try to update
  expected_output = svntest.wc.State(wc_dir, {})
  expected_status.tweak(wc_rev='3')
  expected_status.tweak('A/D/G/rho', status='R ', copied='+', wc_rev='-')
  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status)

  # Now commit merged wc
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/rho': Item(verb='Replacing'),
    })
  expected_status.tweak('A/D/G/rho', status='  ', copied=None, wc_rev='4')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

# use -x -w option for ignoring whitespace during merge
def merge_ignore_whitespace(sbox):
  "ignore whitespace when merging"

  sbox.build()
  wc_dir = sbox.wc_dir

  # commit base version of iota
  file_name = "iota"
  file_path = os.path.join(wc_dir, file_name)
  file_url = sbox.repo_url + '/iota'

  svntest.main.file_write(file_path,
                          "Aa\n"
                          "Bb\n"
                          "Cc\n")
  expected_output = svntest.wc.State(wc_dir, {
      'iota' : Item(verb='Sending'),
      })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        None, None, None, None,
                                        None, None, wc_dir)

  # change the file, mostly whitespace changes + an extra line
  svntest.main.file_write(file_path, "A  a\nBb \n Cc\nNew line in iota\n")
  expected_output = wc.State(wc_dir, { file_name : Item(verb='Sending'), })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak(file_name, wc_rev=3)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Backdate iota to revision 2, so we can merge in the rev 3 changes.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'up', '-r', '2', file_path)
  # Make some local whitespace changes, these should not conflict
  # with the remote whitespace changes as both will be ignored.
  svntest.main.file_write(file_path, "    Aa\nB b\nC c\n")

  # Lines changed only by whitespaces - both in local or remote -
  # should be ignored
  expected_output = wc.State(sbox.wc_dir, { file_name : Item(status='G ') })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.tweak(file_name,
                      contents="    Aa\n"
                               "B b\n"
                               "C c\n"
                               "New line in iota\n")
  expected_status = svntest.actions.get_virginal_state(sbox.wc_dir, 1)
  expected_status.tweak('', status=' M', wc_rev=1)
  expected_status.tweak(file_name, status='M ', wc_rev=2)
  expected_skip = wc.State('', { })

  svntest.actions.run_and_verify_merge(sbox.wc_dir, '2', '3',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, 0,
                                       '-x', '-w')

# use -x --ignore-eol-style option for ignoring eolstyle during merge
def merge_ignore_eolstyle(sbox):
  "ignore eolstyle when merging"

  sbox.build()
  wc_dir = sbox.wc_dir

  # commit base version of iota
  file_name = "iota"
  file_path = os.path.join(wc_dir, file_name)
  file_url = sbox.repo_url + '/iota'

  svntest.main.file_write(file_path,
                          "Aa\r\n"
                          "Bb\r\n"
                          "Cc\r\n",
                          "wb")
  expected_output = svntest.wc.State(wc_dir, {
      'iota' : Item(verb='Sending'),
      })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        None, None, None, None,
                                        None, None, wc_dir)

  # change the file, mostly eol changes + an extra line
  svntest.main.file_write(file_path,
                          "Aa\r"
                          "Bb\n"
                          "Cc\r"
                          "New line in iota\n",
                          "wb")
  expected_output = wc.State(wc_dir, { file_name : Item(verb='Sending'), })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak(file_name, wc_rev=3)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Backdate iota to revision 2, so we can merge in the rev 3 changes.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'up', '-r', '2', file_path)
  # Make some local eol changes, these should not conflict
  # with the remote eol changes as both will be ignored.
  svntest.main.file_write(file_path,
                          "Aa\n"
                          "Bb\r"
                          "Cc\n",
                          "wb")

  # Lines changed only by eolstyle - both in local or remote -
  # should be ignored
  expected_output = wc.State(sbox.wc_dir, { file_name : Item(status='G ') })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.tweak(file_name,
                      contents="Aa\n"
                               "Bb\r"
                               "Cc\n"
                               "New line in iota\n")
  expected_status = svntest.actions.get_virginal_state(sbox.wc_dir, 1)
  expected_status.tweak('', status=' M')
  expected_status.tweak(file_name, status='M ', wc_rev=2)
  expected_skip = wc.State('', { })

  svntest.actions.run_and_verify_merge(sbox.wc_dir, '2', '3',
                                       sbox.repo_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       0, 0,
                                       '-x', '--ignore-eol-style')

#----------------------------------------------------------------------
# Issue 2584
def merge_add_over_versioned_file_conflicts(sbox):
  "conflict from merge of add over versioned file"

  sbox.build()
  wc_dir = sbox.wc_dir

  E_path = os.path.join(wc_dir, 'A', 'B', 'E')
  alpha_path = os.path.join(E_path, 'alpha')
  new_alpha_path = os.path.join(wc_dir, 'A', 'C', 'alpha')

  # Create a new "alpha" file, with enough differences to cause a conflict.
  ### TODO: Leverage inject_conflict_into_wc() here.
  svntest.main.file_write(new_alpha_path, 'new alpha content\n')

  # Add and commit the new "alpha" file, creating revision 2.
  svntest.main.run_svn(None, "add", new_alpha_path)

  expected_output = svntest.wc.State(wc_dir, {
    'A/C/alpha' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/C/alpha' : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_E_path = shorten_path_kludge(E_path)

  # Merge changes from r1:2 into our pre-existing "alpha" file,
  # causing a conflict.
  expected_output = wc.State(short_E_path, {
    'alpha'   : Item(status='C '),
    })
  expected_disk = wc.State('', {
    'alpha'    : Item(""),  # state filled in below
    'beta'    : Item("This is the file 'beta'.\n"),
    })
  expected_status = wc.State(short_E_path, {
    ''       : Item(status=' M', wc_rev=1),
    'alpha'  : Item(status='C ', wc_rev=1),
    'beta'   : Item(status='  ', wc_rev=1),
    })

  inject_conflict_into_expected_state('alpha', expected_disk, expected_status,
                                      "This is the file 'alpha'.\n",
                                      "new alpha content\n", 2)
  expected_skip = wc.State(short_E_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_E_path, '1', '2',
                                       sbox.repo_url + \
                                       '/A/C',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None,
                                       svntest.tree.detect_conflict_files,
                                       ["alpha\.working",
                                        "alpha\.merge-right\.r2",
                                        "alpha\.merge-left\.r0"])

#----------------------------------------------------------------------
# eol-style handling during merge with conflicts, scenario 1:
# when a merge creates a conflict on a file, make sure the file and files
# r<left>, r<right> and .mine are in the eol-style defined for that file.
#
# This test for 'svn update' can be found in update_tests.py as
# conflict_markers_matching_eol.
def merge_conflict_markers_matching_eol(sbox):
  "conflict markers should match the file's eol style"

  sbox.build()
  wc_dir = sbox.wc_dir
  filecount = 1

  mu_path = os.path.join(wc_dir, 'A', 'mu')

  if os.name == 'nt':
    crlf = '\n'
  else:
    crlf = '\r\n'

  # Checkout a second working copy
  wc_backup = sbox.add_wc_path('backup')
  svntest.actions.run_and_verify_svn(None, None, [], 'checkout',
                                     sbox.repo_url, wc_backup)

  # set starting revision
  cur_rev = 1

  expected_disk = svntest.main.greek_state.copy()
  expected_status = svntest.actions.get_virginal_state(wc_dir, cur_rev)
  expected_backup_status = svntest.actions.get_virginal_state(wc_backup,
                                                              cur_rev)

  path_backup = os.path.join(wc_backup, 'A', 'mu')

  # do the test for each eol-style
  for eol, eolchar in zip(['CRLF', 'CR', 'native', 'LF'],
                          [crlf, '\015', '\n', '\012']):
    # rewrite file mu and set the eol-style property.
    svntest.main.file_write(mu_path, "This is the file 'mu'."+ eolchar, 'wb')
    svntest.main.run_svn(None, 'propset', 'svn:eol-style', eol, mu_path)

    expected_disk.add({
      'A/mu' : Item("This is the file 'mu'." + eolchar)
    })
    expected_output = svntest.wc.State(wc_dir, {
      'A/mu' : Item(verb='Sending'),
    })
    expected_status.tweak(wc_rev = cur_rev)
    expected_status.add({
      'A/mu' : Item(status='  ', wc_rev = cur_rev + 1),
    })

    # Commit the original change and note the 'base' revision number
    svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                          expected_status, None,
                                          None, None, None, None, wc_dir)
    cur_rev = cur_rev + 1
    base_rev = cur_rev

    svntest.main.run_svn(None, 'update', wc_backup)

    # Make a local mod to mu
    svntest.main.file_append(mu_path,
                             'Original appended text for mu' + eolchar)

    # Commit the original change and note the 'theirs' revision number
    svntest.main.run_svn(None, 'commit', '-m', 'test log', wc_dir)
    cur_rev = cur_rev + 1
    theirs_rev = cur_rev

    # Make a local mod to mu, will conflict with the previous change
    svntest.main.file_append(path_backup,
                             'Conflicting appended text for mu' + eolchar)

    # Create expected output tree for an update of the wc_backup.
    expected_backup_output = svntest.wc.State(wc_backup, {
      'A/mu' : Item(status='C '),
      })

    # Create expected disk tree for the update.
    expected_backup_disk = expected_disk.copy()

    # verify content of resulting conflicted file
    expected_backup_disk.add({
    'A/mu' : Item(contents= "This is the file 'mu'." + eolchar +
      "<<<<<<< .working" + eolchar +
      "Conflicting appended text for mu" + eolchar +
      "=======" + eolchar +
      "Original appended text for mu" + eolchar +
      ">>>>>>> .merge-right.r" + str(cur_rev) + eolchar),
    })
    # verify content of base(left) file
    expected_backup_disk.add({
    'A/mu.merge-left.r' + str(base_rev) :
      Item(contents= "This is the file 'mu'." + eolchar)
    })
    # verify content of theirs(right) file
    expected_backup_disk.add({
    'A/mu.merge-right.r' + str(theirs_rev) :
      Item(contents= "This is the file 'mu'." + eolchar +
      "Original appended text for mu" + eolchar)
    })
    # verify content of mine file
    expected_backup_disk.add({
    'A/mu.working' : Item(contents= "This is the file 'mu'." +
      eolchar +
      "Conflicting appended text for mu" + eolchar)
    })

    # Create expected status tree for the update.
    expected_backup_status.add({
      'A/mu'   : Item(status='  ', wc_rev=cur_rev),
    })
    expected_backup_status.tweak('A/mu', status='C ')
    expected_backup_status.tweak(wc_rev = cur_rev - 1)
    expected_backup_status.tweak('', status= ' M')

    expected_backup_skip = wc.State('', { })

    svntest.actions.run_and_verify_merge(wc_backup, cur_rev - 1, cur_rev,
                                         sbox.repo_url,
                                         expected_backup_output,
                                         expected_backup_disk,
                                         expected_backup_status,
                                         expected_backup_skip)

    # cleanup for next run
    svntest.main.run_svn(None, 'revert', '-R', wc_backup)
    svntest.main.run_svn(None, 'update', wc_dir)

# eol-style handling during merge, scenario 2:
# if part of that merge is a propchange (add, change, delete) of
# svn:eol-style, make sure the correct eol-style is applied before
# calculating the merge (and conflicts if any)
#
# This test for 'svn update' can be found in update_tests.py as
# update_eolstyle_handling.
def merge_eolstyle_handling(sbox):
  "handle eol-style propchange during merge"

  sbox.build()
  wc_dir = sbox.wc_dir

  mu_path = os.path.join(wc_dir, 'A', 'mu')

  if os.name == 'nt':
    crlf = '\n'
  else:
    crlf = '\r\n'

  # Checkout a second working copy
  wc_backup = sbox.add_wc_path('backup')
  svntest.actions.run_and_verify_svn(None, None, [], 'checkout',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     sbox.repo_url, wc_backup)
  path_backup = os.path.join(wc_backup, 'A', 'mu')

  # Test 1: add the eol-style property and commit, change mu in the second
  # working copy and merge the last revision; there should be no conflict!
  svntest.main.run_svn(None, 'propset', 'svn:eol-style', "CRLF", mu_path)
  svntest.main.run_svn(None,
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       'commit', '-m', 'set eol-style property', wc_dir)

  svntest.main.file_append_binary(path_backup, 'Added new line of text.\012')

  expected_backup_disk = svntest.main.greek_state.copy()
  expected_backup_disk.tweak(
  'A/mu', contents= "This is the file 'mu'." + crlf +
    "Added new line of text." + crlf)
  expected_backup_output = svntest.wc.State(wc_backup, {
    'A/mu' : Item(status='GU'),
    })
  expected_backup_status = svntest.actions.get_virginal_state(wc_backup, 1)
  expected_backup_status.tweak('', status=' M')
  expected_backup_status.tweak('A/mu', status='MM')

  expected_backup_skip = wc.State('', { })

  svntest.actions.run_and_verify_merge(wc_backup, '1', '2', sbox.repo_url,
                                       expected_backup_output,
                                       expected_backup_disk,
                                       expected_backup_status,
                                       expected_backup_skip)

  # Test 2: now change the eol-style property to another value and commit,
  # merge this revision in the still changed mu in the second working copy;
  # there should be a property conflict! (Since this will overwrite a
  # local change to a versioned resource.)
  svntest.main.run_svn(None, 'propset', 'svn:eol-style', "CR", mu_path)
  svntest.main.run_svn(None,
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       'commit', '-m', 'set eol-style property', wc_dir)

  expected_backup_disk = svntest.main.greek_state.copy()
  expected_backup_disk.add({
  'A/mu' : Item(contents= "This is the file 'mu'." + crlf +
    "Added new line of text." + crlf)
  })
  expected_backup_disk.add({
    'A/mu.prej' : Item("Trying to change property 'svn:eol-style' from 'CRLF'"
                       + " to 'CR',\nbut property has been locally added with"
                       + " value 'CRLF'\n")})
  expected_backup_output = svntest.wc.State(wc_backup, {
    'A/mu' : Item(status='GC'),
    })
  expected_backup_status = svntest.actions.get_virginal_state(wc_backup, 1)
  expected_backup_status.tweak('', status=' M')
  expected_backup_status.tweak('A/mu', status='MC')
  svntest.actions.run_and_verify_merge(wc_backup, '2', '3', sbox.repo_url,
                                        expected_backup_output,
                                        expected_backup_disk,
                                        expected_backup_status,
                                        expected_backup_skip)

  # Test 3: now delete the eol-style property and commit, merge this revision
  # in the still changed mu in the second working copy; there should be no
  # conflict! (after marking mu resolved from Test 2)
  # EOL of mu should be unchanged (=CRLF).
  svntest.main.run_svn(None, 'propdel', 'svn:eol-style', mu_path)
  svntest.main.run_svn(None,
                       '--username', svntest.main.wc_author,
                       '--password', svntest.main.wc_passwd,
                       'commit', '-m', 'del eol-style property', wc_dir)

  expected_backup_disk = svntest.main.greek_state.copy()
  expected_backup_disk.add({
  'A/mu' : Item(contents= "This is the file 'mu'." + crlf +
    "Added new line of text." + crlf)
  })
  expected_backup_output = svntest.wc.State(wc_backup, {
    'A/mu' : Item(status=' G'),
    })
  expected_backup_status = svntest.actions.get_virginal_state(wc_backup, 1)
  expected_backup_status.tweak('', status=' M')
  expected_backup_status.tweak('A/mu', status='M ')
  svntest.main.run_svn(None, 'resolved', path_backup)
  svntest.actions.run_and_verify_merge(wc_backup, '3', '4', sbox.repo_url,
                                       expected_backup_output,
                                       expected_backup_disk,
                                       expected_backup_status,
                                       expected_backup_skip)

def create_deep_trees(wc_dir):
  """Create A/B/F/E by moving A/B/E to A/B/F/E.
     Copy A/B/F/E to A/B/F/E1.
     Copy A/B to A/copy-of-B, and return the expected status.
     At the end of this function WC would be at r4"""

  A_path = os.path.join(wc_dir, 'A')
  A_B_path = os.path.join(A_path, 'B')
  A_B_E_path = os.path.join(A_B_path, 'E')
  A_B_F_path = os.path.join(A_B_path, 'F')
  A_B_F_E_path = os.path.join(A_B_F_path, 'E')
  A_B_F_E1_path = os.path.join(A_B_F_path, 'E1')

  # Deepen the directory structure we're working with by moving E to
  # underneath F and committing, creating revision 2.
  svntest.main.run_svn(None, 'mv', '-g', A_B_E_path, A_B_F_path)
  expected_output = wc.State(wc_dir, {
    'A/B/E'   : Item(verb='Deleting'),
    'A/B/F/E' : Item(verb='Adding')
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.remove('A/B/E', 'A/B/E/alpha', 'A/B/E/beta')
  expected_status.add({
    'A/B/F/E'       : Item(status='  ', wc_rev=2),
    'A/B/F/E/alpha' : Item(status='  ', wc_rev=2),
    'A/B/F/E/beta'  : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  svntest.main.run_svn(None, 'cp', '-g', A_B_F_E_path, A_B_F_E1_path)
  expected_output = wc.State(wc_dir, {
    'A/B/F/E1' : Item(verb='Adding')
    })
  expected_status.add({
    'A/B/F/E1'       : Item(status='  ', wc_rev=3),
    'A/B/F/E1/alpha' : Item(status='  ', wc_rev=3),
    'A/B/F/E1/beta'  : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Bring the entire WC up to date with rev 3.
  svntest.actions.run_and_verify_svn(None, None, [], 'update', wc_dir)
  expected_status.tweak(wc_rev=3)

  # Copy B and commit, creating revision 4.
  copy_of_B_path = os.path.join(A_path, 'copy-of-B')
  svntest.main.run_svn(None, "cp", "-g", A_B_path, copy_of_B_path)

  expected_output = svntest.wc.State(wc_dir, {
    'A/copy-of-B' : Item(verb='Adding'),
    })
  expected_status.add({
    'A/copy-of-B'            : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F'          : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F/E'        : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F/E/alpha'  : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F/E/beta'   : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F/E1'       : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F/E1/alpha' : Item(status='  ', wc_rev=4),
    'A/copy-of-B/F/E1/beta'  : Item(status='  ', wc_rev=4),
    'A/copy-of-B/lambda'     : Item(status='  ', wc_rev=4),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Bring the entire WC up to date with rev 4.
  svntest.actions.run_and_verify_svn(None, None, [], 'update', wc_dir)
  expected_status.tweak(wc_rev=4)
  return expected_status

def avoid_repeated_merge_using_inherited_merge_info(sbox):
  "use inherited mergeinfo to avoid repeated merge"

  sbox.build()
  wc_dir = sbox.wc_dir

  A_path = os.path.join(wc_dir, 'A')
  A_B_path = os.path.join(A_path, 'B')
  A_B_E_path = os.path.join(A_B_path, 'E')
  A_B_F_path = os.path.join(A_B_path, 'F')
  copy_of_B_path = os.path.join(A_path, 'copy-of-B')

  # Create a deeper directory structure.
  expected_status = create_deep_trees(wc_dir)

  # Edit alpha and commit it, creating revision 5.
  alpha_path = os.path.join(A_B_F_path, 'E', 'alpha')
  new_content_for_alpha = 'new content to alpha\n'
  svntest.main.file_write(alpha_path, new_content_for_alpha)

  expected_output = svntest.wc.State(wc_dir, {
    'A/B/F/E/alpha' : Item(verb='Sending'),
    })
  expected_status.tweak('A/B/F/E/alpha', wc_rev=5)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_copy_of_B_path = shorten_path_kludge(copy_of_B_path)

  # Merge changes from rev 5 of B (to alpha) into copy_of_B.
  expected_output = wc.State(short_copy_of_B_path, {
    'F/E/alpha'   : Item(status='U '),
    })
  expected_status = wc.State(short_copy_of_B_path, {
    ''           : Item(status=' M', wc_rev=4),
    'F/E'        : Item(status='  ', wc_rev=4),
    'F/E/alpha'  : Item(status='M ', wc_rev=4),
    'F/E/beta'   : Item(status='  ', wc_rev=4),
    'F/E1'       : Item(status='  ', wc_rev=4),
    'F/E1/alpha' : Item(status='  ', wc_rev=4),
    'F/E1/beta'  : Item(status='  ', wc_rev=4),
    'lambda'     : Item(status='  ', wc_rev=4),
    'F'          : Item(status='  ', wc_rev=4),
    })
  expected_disk = wc.State('', {
    ''           : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-3,5'}),
    'F/E'        : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:1'}),
    'F/E/alpha'  : Item(new_content_for_alpha),
    'F/E/beta'   : Item("This is the file 'beta'.\n"),
    'F/E1'       : Item(props={SVN_PROP_MERGE_INFO :
                               '/A/B/E:1\n/A/B/F/E:2\n'}),
    'F/E1/alpha' : Item("This is the file 'alpha'.\n"),
    'F/E1/beta'  : Item("This is the file 'beta'.\n"),
    'F'          : Item(),
    'lambda'     : Item("This is the file 'lambda'.\n")
    })
  expected_skip = wc.State(short_copy_of_B_path, { })
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_copy_of_B_path, '4', '5',
                                       sbox.repo_url + \
                                       '/A/B',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None,
                                       None,
                                       None,
                                       None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Commit the result of the merge, creating revision 6.
  expected_output = svntest.wc.State(copy_of_B_path, {
    ''          : Item(verb='Sending'),
    'F/E/alpha' : Item(verb='Sending'),
    })
  svntest.actions.run_and_verify_commit(short_copy_of_B_path, expected_output,
                                        None, None,
                                        None, None, None, None, wc_dir)

  # Update the WC to bring /A/copy_of_B/F from rev 4 to rev 6.
  # Without this update, a subsequent merge will not find any merge
  # info for /A/copy_of_B/F -- nor its parent dir in the repos -- at
  # rev 4.  Mergeinfo wasn't introduced until rev 6.
  copy_of_B_F_E_path = os.path.join(copy_of_B_path, 'F', 'E')
  svntest.actions.run_and_verify_svn(None, None, [], 'update', wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_copy_of_B_F_E_path = shorten_path_kludge(copy_of_B_F_E_path)

  # Attempt to re-merge changes to alpha from rev 4.  Use the merge
  # info inherited from the grandparent (copy-of-B) of our merge
  # target (/A/copy-of-B/F/E) to avoid a repeated merge.
  expected_output = wc.State(copy_of_B_F_E_path, { })
  expected_status = wc.State(short_copy_of_B_F_E_path, {
    ''      : Item(status='  ', wc_rev=6),
    'alpha' : Item(status='  ', wc_rev=6),
    'beta'  : Item(status='  ', wc_rev=6),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:1'}),
    'alpha'   : Item(new_content_for_alpha),
    'beta'    : Item("This is the file 'beta'.\n"),
    })
  expected_skip = wc.State(short_copy_of_B_F_E_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_copy_of_B_F_E_path, '4', '5',
                                       sbox.repo_url + '/A/B/F/E',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None,
                                       None,
                                       None,
                                       None,
                                       None, 1)

def avoid_repeated_merge_on_subtree_with_merge_info(sbox):
  "use subtree's mergeinfo to avoid repeated merge"
  # Create deep trees A/B/F/E and A/B/F/E1 and copy A/B to A/copy-of-B
  # with the help of 'create_deep_trees'
  # As /A/copy-of-B/F/E1 is not a child of /A/copy-of-B/F/E,
  # set_path should not be called on /A/copy-of-B/F/E1 while
  # doing a implicit subtree merge on /A/copy-of-B/F/E.
  sbox.build()
  wc_dir = sbox.wc_dir

  A_path = os.path.join(wc_dir, 'A')
  A_B_path = os.path.join(A_path, 'B')
  A_B_E_path = os.path.join(A_B_path, 'E')
  A_B_F_path = os.path.join(A_B_path, 'F')
  A_B_F_E_path = os.path.join(A_B_F_path, 'E')
  copy_of_B_path = os.path.join(A_path, 'copy-of-B')

  # Create a deeper directory structure.
  expected_status = create_deep_trees(wc_dir)

  # Edit alpha and commit it, creating revision 5.
  alpha_path = os.path.join(A_B_F_E_path, 'alpha')
  new_content_for_alpha1 = 'new content to alpha\n'
  svntest.main.file_write(alpha_path, new_content_for_alpha1)

  expected_output = svntest.wc.State(wc_dir, {
    'A/B/F/E/alpha' : Item(verb='Sending'),
    })
  expected_status.tweak('A/B/F/E/alpha', wc_rev=5)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)


  for path_and_mergeinfo in (('E', '/A/B/E:1\n/A/B/F/E:5\n'),
                             ('E1', '/A/B/E:1\n/A/B/F/E:2,5\n')):
    path_name = os.path.join(copy_of_B_path, 'F', path_and_mergeinfo[0])
    # Search for the comment entitled "The Merge Kluge" elsewhere in
    # this file, to understand why we shorten and chdir() below.
    short_path_name = shorten_path_kludge(path_name)

    # Merge r5 to path_name.
    expected_output = wc.State(short_path_name, {
      'alpha'   : Item(status='U '),
      })
    expected_status = wc.State(short_path_name, {
      ''      : Item(status=' M', wc_rev=4),
      'alpha' : Item(status='M ', wc_rev=4),
      'beta'  : Item(status='  ', wc_rev=4),
      })
    expected_disk = wc.State('', {
      ''        : Item(props={SVN_PROP_MERGE_INFO : path_and_mergeinfo[1]}),
      'alpha'   : Item(new_content_for_alpha1),
      'beta'    : Item("This is the file 'beta'.\n"),
      })
    expected_skip = wc.State(short_path_name, { })
    saved_cwd = os.getcwd()

    os.chdir(svntest.main.work_dir)
    svntest.actions.run_and_verify_merge(short_path_name, '4', '5',
                                         sbox.repo_url + '/A/B/F/E',
                                         expected_output,
                                         expected_disk,
                                         expected_status,
                                         expected_skip,
                                         None,
                                         None,
                                         None,
                                         None,
                                         None, 1)
    os.chdir(saved_cwd)

    # Commit the result of the merge, creating new revision.
    expected_output = svntest.wc.State(path_name, {
      ''      : Item(verb='Sending'),
      'alpha' : Item(verb='Sending'),
      })
    svntest.actions.run_and_verify_commit(short_path_name,
                                          expected_output, None, None,
                                          None, None, None, None, wc_dir)

  # Edit A/B/F/E/alpha and commit it, creating revision 8.
  new_content_for_alpha = 'new content to alpha\none more line\n'
  svntest.main.file_write(alpha_path, new_content_for_alpha)

  expected_output = svntest.wc.State(A_B_F_E_path, {
    'alpha' : Item(verb='Sending'),
    })
  expected_status = wc.State(A_B_F_E_path, {
    ''      : Item(status='  ', wc_rev=4),
    'alpha' : Item(status='  ', wc_rev=8),
    'beta'  : Item(status='  ', wc_rev=4),
    })
  svntest.actions.run_and_verify_commit(A_B_F_E_path, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_copy_of_B_path = shorten_path_kludge(copy_of_B_path)

  # Update the WC to bring /A/copy_of_B to rev 8.
  # Without this update expected_status tree would be cumbersome to
  # understand.
  svntest.actions.run_and_verify_svn(None, None, [], 'update', wc_dir)

  # Merge changes from rev 4:8 of A/B into A/copy_of_B.
  expected_output = wc.State(short_copy_of_B_path, {
    'F/E/alpha' : Item(status='U ')
    })
  expected_status = wc.State(short_copy_of_B_path, {
    # When we merge multiple sub-targets, we record mergeinfo on each
    # child.
    ''           : Item(status=' M', wc_rev=8),
    'F/E'        : Item(status=' M', wc_rev=8),
    'F/E/alpha'  : Item(status='M ', wc_rev=8),
    'F/E/beta'   : Item(status='  ', wc_rev=8),
    'F/E1'       : Item(status='  ', wc_rev=8),
    'F/E1/alpha' : Item(status='  ', wc_rev=8),
    'F/E1/beta'  : Item(status='  ', wc_rev=8),
    'lambda'     : Item(status='  ', wc_rev=8),
    'F'          : Item(status='  ', wc_rev=8)
    })
  expected_disk = wc.State('', {
    ''           : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-3,5-8'}),
    'F/E'        : Item(props={SVN_PROP_MERGE_INFO :
                               '/A/B/E:1\n/A/B/F/E:5-8\n'}),
    'F/E/alpha'  : Item(new_content_for_alpha),
    'F/E/beta'   : Item("This is the file 'beta'.\n"),
    'F'          : Item(),
    'F/E1'       : Item(props={SVN_PROP_MERGE_INFO :
                               '/A/B/E:1\n/A/B/F/E:2,5\n'}),
    'F/E1/alpha' : Item(new_content_for_alpha1),
    'F/E1/beta'  : Item("This is the file 'beta'.\n"),
    'lambda'     : Item("This is the file 'lambda'.\n")
    })
  expected_skip = wc.State(short_copy_of_B_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_copy_of_B_path, '4', '8',
                                       sbox.repo_url + '/A/B',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None,
                                       None,
                                       None,
                                       None,
                                       None, 1)

def tweak_src_then_merge_to_dest(sbox, src_path, dst_path,
                                 canon_src_path, contents, cur_rev):
  """Edit src and commit it. This results in new_rev.
   Merge new_rev to dst_path. Return new_rev."""

  wc_dir = sbox.wc_dir
  new_rev = cur_rev + 1
  svntest.main.file_write(src_path, contents)

  expected_output = svntest.wc.State(src_path, {
    '': Item(verb='Sending'),
    })

  expected_status = wc.State(src_path,
                             { '': Item(wc_rev=new_rev, status='  ')})

  svntest.actions.run_and_verify_commit(src_path, expected_output,
                                        expected_status, None,
                                        None, None, None, None, src_path)

  # Update the WC to new_rev so that it would be easier to expect everyone
  # to be at new_rev.
  svntest.actions.run_and_verify_svn(None, None, [], 'update', wc_dir)

  # Merge new_rev of src_path to dst_path.

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_dst_path = shorten_path_kludge(dst_path)
  expected_status = wc.State(dst_path,
                             { '': Item(wc_rev=new_rev, status='MM')})
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)

  merge_url = sbox.repo_url + '/' + canon_src_path
  if sys.platform == 'win32':
    merge_url = merge_url.replace('\\', '/')

  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(new_rev, 'U    ' +
                                                           short_dst_path +
                                                           '\n'),
                                     [],
                                     'merge', '-c', str(new_rev),
                                     merge_url,
                                     short_dst_path)
  os.chdir(saved_cwd)

  svntest.actions.run_and_verify_status(dst_path, expected_status)

  return new_rev

def obey_reporter_api_semantics_while_doing_subtree_merges(sbox):
  "drive reporter api in depth first order"

  # Copy /A/D to /A/copy-of-D it results in rONE.
  # Create children at different hierarchies having some merge-info
  # to test the set_path calls on a reporter in a depth-first order.
  # On all 'file' descendants of /A/copy-of-D/ we run merges.
  # We create /A/D/umlaut directly over URL it results in rev rTWO.
  # When we merge rONE+1:TWO of /A/D on /A/copy-of-D it should merge smoothly.

  sbox.build()
  wc_dir = sbox.wc_dir

  A_path = os.path.join(wc_dir, 'A')
  A_D_path = os.path.join(wc_dir, 'A', 'D')
  copy_of_A_D_path = os.path.join(wc_dir, 'A', 'copy-of-D')

  svntest.main.run_svn(None, "cp", "-g", A_D_path, copy_of_A_D_path)

  expected_output = svntest.wc.State(wc_dir, {
    'A/copy-of-D' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/copy-of-D'         : Item(status='  ', wc_rev=2),
    'A/copy-of-D/G'       : Item(status='  ', wc_rev=2),
    'A/copy-of-D/G/pi'    : Item(status='  ', wc_rev=2),
    'A/copy-of-D/G/rho'   : Item(status='  ', wc_rev=2),
    'A/copy-of-D/G/tau'   : Item(status='  ', wc_rev=2),
    'A/copy-of-D/H'       : Item(status='  ', wc_rev=2),
    'A/copy-of-D/H/chi'   : Item(status='  ', wc_rev=2),
    'A/copy-of-D/H/omega' : Item(status='  ', wc_rev=2),
    'A/copy-of-D/H/psi'   : Item(status='  ', wc_rev=2),
    'A/copy-of-D/gamma'   : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)


  cur_rev = 2
  for path in (["A", "D", "G", "pi"],
               ["A", "D", "G", "rho"],
               ["A", "D", "G", "tau"],
               ["A", "D", "H", "chi"],
               ["A", "D", "H", "omega"],
               ["A", "D", "H", "psi"],
               ["A", "D", "gamma"]):
    path_name = os.path.join(wc_dir, *path)
    canon_path_name = os.path.join(*path)
    path[1] = "copy-of-D"
    copy_of_path_name = os.path.join(wc_dir, *path)
    var_name = 'new_content_for_' + path[len(path) - 1]
    file_contents = "new content to " + path[len(path) - 1] + "\n"
    globals()[var_name] = file_contents
    cur_rev = tweak_src_then_merge_to_dest(sbox, path_name,
                                           copy_of_path_name, canon_path_name,
                                           file_contents, cur_rev)

  copy_of_A_D_wc_rev = cur_rev
  svntest.actions.run_and_verify_svn(None,
                                     ['\n',
                                      'Committed revision ' + str(cur_rev+1) + '.\n'],
                                     [],
                                     'mkdir', sbox.repo_url + '/A/D/umlaut',
                                     '-m', "log msg")
  rev_to_merge_to_copy_of_D = cur_rev + 1

  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_copy_of_A_D_path = shorten_path_kludge(copy_of_A_D_path)

  # All the file descendants of /A/copy-of-D/ have already been merged
  # so the only notification we expect is for the added 'umlaut'.
  expected_output = wc.State(short_copy_of_A_D_path, {
    'umlaut'  : Item(status='A '),
    })

  # All the local svn:mergeinfo under A/copy-of-D elides
  # to A/copy-of-D.
  expected_status = wc.State(short_copy_of_A_D_path, {
    ''        : Item(status=' M', wc_rev=copy_of_A_D_wc_rev),
    'G'       : Item(status='  ', wc_rev=copy_of_A_D_wc_rev),
    'G/pi'    : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'G/rho'   : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'G/tau'   : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'H'       : Item(status='  ', wc_rev=copy_of_A_D_wc_rev),
    'H/chi'   : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'H/omega' : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'H/psi'   : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'gamma'   : Item(status='M ', wc_rev=copy_of_A_D_wc_rev),
    'umlaut'  : Item(status='A ', copied='+', wc_rev='-'),
    })

  merged_rangelist = "1,3-%d" % rev_to_merge_to_copy_of_D


  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/D:' + merged_rangelist}),
    'G'       : Item(),
    'G/pi'    : Item(new_content_for_pi),
    'G/rho'   : Item(new_content_for_rho),
    'G/tau'   : Item(new_content_for_tau),
    'H'       : Item(),
    'H/chi'   : Item(new_content_for_chi,),
    'H/omega' : Item(new_content_for_omega,),
    'H/psi'   : Item(new_content_for_psi,),
    'gamma'   : Item(new_content_for_gamma,),
    'umlaut'  : Item(),
    })
  expected_skip = wc.State(short_copy_of_A_D_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_copy_of_A_D_path,
                                       2,
                                       str(rev_to_merge_to_copy_of_D),
                                       sbox.repo_url + '/A/D',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None,
                                       None,
                                       None,
                                       None,
                                       None, 1)

def setup_branch(sbox, branch_only = False, nbr_of_branches = 1):
  '''Starting with standard greek tree, copy 'A' NBR_OF_BRANCHES times
  to A_COPY, A_COPY_2, A_COPY_3, and so on.  Then make four modifications
  (setting file contents to "New content") under A:
  r(2 + NBR_OF_BRANCHES) - A/D/H/psi
  r(3 + NBR_OF_BRANCHES) - A/D/G/rho
  r(4 + NBR_OF_BRANCHES) - A/B/E/beta
  r(5 + NBR_OF_BRANCHES) - A/D/H/omega'''

  wc_dir = sbox.wc_dir

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_disk = svntest.main.greek_state.copy()

  def copy_A(dest_name, rev):
    expected = svntest.verify.UnorderedOutput(
      ["A    " + os.path.join(wc_dir, dest_name, "B") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "B", "lambda") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "B", "E") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "B", "E", "alpha") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "B", "E", "beta") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "B", "F") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "mu") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "C") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "gamma") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "G") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "G", "pi") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "G", "rho") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "G", "tau") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "H") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "H", "chi") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "H", "omega") + "\n",
       "A    " + os.path.join(wc_dir, dest_name, "D", "H", "psi") + "\n",
       "Checked out revision " + str(rev - 1) + ".\n",
       "A         " + os.path.join(wc_dir, dest_name) + "\n"])
    expected_status.add({
      dest_name + "/B"         : Item(status='  ', wc_rev=rev),
      dest_name + "/B/lambda"  : Item(status='  ', wc_rev=rev),
      dest_name + "/B/E"       : Item(status='  ', wc_rev=rev),
      dest_name + "/B/E/alpha" : Item(status='  ', wc_rev=rev),
      dest_name + "/B/E/beta"  : Item(status='  ', wc_rev=rev),
      dest_name + "/B/F"       : Item(status='  ', wc_rev=rev),
      dest_name + "/mu"        : Item(status='  ', wc_rev=rev),
      dest_name + "/C"         : Item(status='  ', wc_rev=rev),
      dest_name + "/D"         : Item(status='  ', wc_rev=rev),
      dest_name + "/D/gamma"   : Item(status='  ', wc_rev=rev),
      dest_name + "/D/G"       : Item(status='  ', wc_rev=rev),
      dest_name + "/D/G/pi"    : Item(status='  ', wc_rev=rev),
      dest_name + "/D/G/rho"   : Item(status='  ', wc_rev=rev),
      dest_name + "/D/G/tau"   : Item(status='  ', wc_rev=rev),
      dest_name + "/D/H"       : Item(status='  ', wc_rev=rev),
      dest_name + "/D/H/chi"   : Item(status='  ', wc_rev=rev),
      dest_name + "/D/H/omega" : Item(status='  ', wc_rev=rev),
      dest_name + "/D/H/psi"   : Item(status='  ', wc_rev=rev),
      dest_name                : Item(status='  ', wc_rev=rev)})
    if rev < 3:
      copy_mergeinfo = '/A:1'
    else:
      copy_mergeinfo  = '/A:1-' + str(rev - 1)
    expected_disk.add({
      dest_name : Item(props={SVN_PROP_MERGE_INFO : copy_mergeinfo}),
      dest_name + '/B'         : Item(),
      dest_name + '/B/lambda'  : Item("This is the file 'lambda'.\n"),
      dest_name + '/B/E'       : Item(),
      dest_name + '/B/E/alpha' : Item("This is the file 'alpha'.\n"),
      dest_name + '/B/E/beta'  : Item("This is the file 'beta'.\n"),
      dest_name + '/B/F'       : Item(),
      dest_name + '/mu'        : Item("This is the file 'mu'.\n"),
      dest_name + '/C'         : Item(),
      dest_name + '/D'         : Item(),
      dest_name + '/D/gamma'   : Item("This is the file 'gamma'.\n"),
      dest_name + '/D/G'       : Item(),
      dest_name + '/D/G/pi'    : Item("This is the file 'pi'.\n"),
      dest_name + '/D/G/rho'   : Item("This is the file 'rho'.\n"),
      dest_name + '/D/G/tau'   : Item("This is the file 'tau'.\n"),
      dest_name + '/D/H'       : Item(),
      dest_name + '/D/H/chi'   : Item("This is the file 'chi'.\n"),
      dest_name + '/D/H/omega' : Item("This is the file 'omega'.\n"),
      dest_name + '/D/H/psi'   : Item("This is the file 'psi'.\n"),
      })

    # Make a branch A_COPY to merge into.
    svntest.actions.run_and_verify_svn(None, expected, [], 'copy',
                                       sbox.repo_url + "/A",
                                       os.path.join(wc_dir,
                                                    dest_name))

    expected_output = wc.State(wc_dir, {dest_name : Item(verb='Adding')})
    svntest.actions.run_and_verify_commit(wc_dir,
                                          expected_output,
                                          expected_status,
                                          None,
                                          None, None, None, None,
                                          wc_dir)
  for i in range(nbr_of_branches):
    if i == 0:
      copy_A('A_COPY', i + 2)
    else:
      copy_A('A_COPY_' + str(i + 1), i + 2)

  if (branch_only):
    return expected_disk, expected_status

  # Make some changes under A which we'll later merge under A_COPY:

  # r(nbr_of_branches + 2) - modify and commit A/D/H/psi
  svntest.main.file_write(os.path.join(wc_dir, "A", "D", "H", "psi"),
                          "New content")
  expected_output = wc.State(wc_dir, {'A/D/H/psi' : Item(verb='Sending')})
  expected_status.tweak('A/D/H/psi', wc_rev=nbr_of_branches + 2)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  expected_disk.tweak('A/D/H/psi', contents="New content")

  # r(nbr_of_branches + 3) - modify and commit A/D/G/rho
  svntest.main.file_write(os.path.join(wc_dir, "A", "D", "G", "rho"),
                          "New content")
  expected_output = wc.State(wc_dir, {'A/D/G/rho' : Item(verb='Sending')})
  expected_status.tweak('A/D/G/rho', wc_rev=nbr_of_branches + 3)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  expected_disk.tweak('A/D/G/rho', contents="New content")

  # r(nbr_of_branches + 4) - modify and commit A/B/E/beta
  svntest.main.file_write(os.path.join(wc_dir, "A", "B", "E", "beta"),
                          "New content")
  expected_output = wc.State(wc_dir, {'A/B/E/beta' : Item(verb='Sending')})
  expected_status.tweak('A/B/E/beta', wc_rev=nbr_of_branches + 4)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  expected_disk.tweak('A/B/E/beta', contents="New content")

  # r(nbr_of_branches + 5) - modify and commit A/D/H/omega
  svntest.main.file_write(os.path.join(wc_dir, "A", "D", "H", "omega"),
                          "New content")
  expected_output = wc.State(wc_dir, {'A/D/H/omega' : Item(verb='Sending')})
  expected_status.tweak('A/D/H/omega', wc_rev=nbr_of_branches + 5)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  expected_disk.tweak('A/D/H/omega', contents="New content")

  return expected_disk, expected_status


def mergeinfo_inheritance(sbox):
  "target inherits mergeinfo from nearest ancestor"

  # Test for Issues #2733 and #2734.
  #
  # When the target of a merge has no explicit mergeinfo and the merge
  # would result in mergeinfo being added to the target which...
  #
  #   ...is a subset of the *local*  mergeinfo on one of the target's
  #   ancestors (it's nearest ancestor takes precedence), then the merge is
  #   not repeated and no mergeinfo should be set on the target (Issue #2734).
  #
  # OR
  #
  #   ...is not a subset it's nearest ancestor, the target should inherit the
  #   non-inersecting mergeinfo (local or committed, the former takes
  #   precedence) from it's nearest ancestor (Issue #2733).

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox)

  # Some paths we'll care about
  A_COPY_path      = os.path.join(wc_dir, "A_COPY")
  B_COPY_path      = os.path.join(wc_dir, "A_COPY", "B")
  beta_COPY_path   = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  E_COPY_path      = os.path.join(wc_dir, "A_COPY", "B", "E")
  omega_COPY_path  = os.path.join(wc_dir, "A_COPY", "D", "H", "omega")
  D_COPY_path      = os.path.join(wc_dir, "A_COPY", "D")
  G_COPY_path      = os.path.join(wc_dir, "A_COPY", "D", "G")

  # Now start merging...

  # Merge r4 into A_COPY/D/
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_D_COPY_path = shorten_path_kludge(D_COPY_path)
  expected_output = wc.State(short_D_COPY_path, {
    'G/rho' : Item(status='U '),
    })
  expected_status = wc.State(short_D_COPY_path, {
    ''        : Item(status=' M', wc_rev=2),
    'G'       : Item(status='  ', wc_rev=2),
    'G/pi'    : Item(status='  ', wc_rev=2),
    'G/rho'   : Item(status='M ', wc_rev=2),
    'G/tau'   : Item(status='  ', wc_rev=2),
    'H'       : Item(status='  ', wc_rev=2),
    'H/chi'   : Item(status='  ', wc_rev=2),
    'H/psi'   : Item(status='  ', wc_rev=2),
    'H/omega' : Item(status='  ', wc_rev=2),
    'gamma'   : Item(status='  ', wc_rev=2),
    })
  # We test issue #2733 here (with a directory as the merge target).
  # r1 should be inherited from 'A_COPY'.
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1,4'}),
    'G'       : Item(),
    'G/pi'    : Item("This is the file 'pi'.\n"),
    'G/rho'   : Item("New content"),
    'G/tau'   : Item("This is the file 'tau'.\n"),
    'H'       : Item(),
    'H/chi'   : Item("This is the file 'chi'.\n"),
    'H/psi'   : Item("This is the file 'psi'.\n"),
    'H/omega' : Item("This is the file 'omega'.\n"),
    'gamma'   : Item("This is the file 'gamma'.\n")
    })
  expected_skip = wc.State(short_D_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '3', '4',
                                       sbox.repo_url + \
                                       '/A/D',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Merge r4 again, this time into A_COPY/D/G.  An ancestor directory
  # (A_COPY/D) exists with identical local mergeinfo, so the merge
  # should not be repeated.  We test issue #2734 here with (with a
  # directory as the merge target).
  short_G_COPY_path = shorten_path_kludge(G_COPY_path)
  expected_output = wc.State(short_G_COPY_path, { })
  expected_status = wc.State(short_G_COPY_path, {
    ''    : Item(status='  ', wc_rev=2),
    'pi'  : Item(status='  ', wc_rev=2),
    'rho' : Item(status='M ', wc_rev=2),
    'tau' : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    'pi'  : Item("This is the file 'pi'.\n"),
    'rho' : Item("New content"),
    'tau' : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State(short_G_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_G_COPY_path, '3', '4',
                                       sbox.repo_url + \
                                       '/A/D/G',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)
  # Merge r5 into A_COPY/B.  Again, r1 should be inherited from
  # A_COPY (Issue #2733)
  short_B_COPY_path = shorten_path_kludge(B_COPY_path)
  expected_output = wc.State(short_B_COPY_path, {
    'E/beta' : Item(status='U '),
    })
  expected_status = wc.State(short_B_COPY_path, {
    ''        : Item(status=' M', wc_rev=2),
    'E'       : Item(status='  ', wc_rev=2),
    'E/alpha' : Item(status='  ', wc_rev=2),
    'E/beta'  : Item(status='M ', wc_rev=2),
    'lambda'  : Item(status='  ', wc_rev=2),
    'F'       : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,5'}),
    'E'       : Item(),
    'E/alpha' : Item("This is the file 'alpha'.\n"),
    'E/beta'  : Item("New content"),
    'F'       : Item(),
    'lambda'  : Item("This is the file 'lambda'.\n")
    })
  expected_skip = wc.State(short_B_COPY_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_B_COPY_path, '4', '5',
                                       sbox.repo_url + \
                                       '/A/B',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Merge r5 again, this time into A_COPY/B/E/beta.  An ancestor
  # directory (A_COPY/B) exists with identical local mergeinfo, so
  # the merge should not be repeated (Issue #2734 with a file as the
  # merge target).
  short_beta_COPY_path = shorten_path_kludge(beta_COPY_path)
  expected_skip = wc.State(short_beta_COPY_path, { })
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None, [], [], 'merge', '-c5',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)
  os.chdir(saved_cwd)

  # The merge wasn't repeated so beta shouldn't have any mergeinfo.
  # We are implicitly testing that without looking at the prop value
  # itself, just beta's prop modification status.
  expected_status = wc.State(beta_COPY_path, {
    ''        : Item(status='M ', wc_rev=2),
    })
  svntest.actions.run_and_verify_status(beta_COPY_path, expected_status)

  # Merge r3 into A_COPY.  A_COPY's two descendents with mergeinfo,
  # A_COPY/B/E/beta and A_COPY/D/G/rho must have complete mergeinfo
  # so they both should pick up r3 too.
  short_A_COPY_path = shorten_path_kludge(A_COPY_path)
  expected_output = wc.State(short_A_COPY_path, {
    'D/H/psi'   : Item(status='U '),
    })
  expected_status = wc.State(short_A_COPY_path, {
    ''          : Item(status=' M', wc_rev=2),
    'B'         : Item(status=' M', wc_rev=2),
    'mu'        : Item(status='  ', wc_rev=2),
    'B/E'       : Item(status='  ', wc_rev=2),
    'B/E/alpha' : Item(status='  ', wc_rev=2),
    'B/E/beta'  : Item(status='M ', wc_rev=2),
    'B/lambda'  : Item(status='  ', wc_rev=2),
    'B/F'       : Item(status='  ', wc_rev=2),
    'C'         : Item(status='  ', wc_rev=2),
    'D'         : Item(status=' M', wc_rev=2),
    'D/G'       : Item(status='  ', wc_rev=2),
    'D/G/pi'    : Item(status='  ', wc_rev=2),
    'D/G/rho'   : Item(status='M ', wc_rev=2),
    'D/G/tau'   : Item(status='  ', wc_rev=2),
    'D/gamma'   : Item(status='  ', wc_rev=2),
    'D/H'       : Item(status='  ', wc_rev=2),
    'D/H/chi'   : Item(status='  ', wc_rev=2),
    'D/H/psi'   : Item(status='M ', wc_rev=2),
    'D/H/omega' : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,3'}),
    'B'         : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,3,5'}),
    'mu'        : Item("This is the file 'mu'.\n"),
    'B/E'       : Item(),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("New content"),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/F'       : Item(),
    'C'         : Item(),
    'D'         : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1,3-4'}),
    'D/G'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("New content"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D/H'       : Item(),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
    'D/H/psi'   : Item("New content"),
    'D/H/omega' : Item("This is the file 'omega'.\n"),
    })
  expected_skip = wc.State(short_A_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY_path, '2', '3',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Merge r6 into A_COPY/D/H/omega, it should inherit it's nearest
  # ancestor's (A_COPY/D) mergeinfo (Issue #2733 with a file as the
  # merge target).
  short_omega_COPY_path = shorten_path_kludge(omega_COPY_path)
  expected_skip = wc.State(short_omega_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(6,
                                       'U    ' + short_omega_COPY_path + '\n'),
                                     [], 'merge', '-c6',
                                     sbox.repo_url + '/A/D/H/omega',
                                     short_omega_COPY_path)
  os.chdir(saved_cwd)

  # Check that mergeinfo was properly set on A_COPY/D/H/omega
  svntest.actions.run_and_verify_svn(None,
                                     ["/A/D/H/omega:1,3-4,6\n"],
                                     [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     omega_COPY_path)

  # Given a merge target *without* any of the following:
  #
  #   1) Explicit mergeinfo set on itself in the WC
  #   2) Any WC ancestor to inherit mergeinfo from
  #   3) Any mergeinfo for the target in the repository
  #
  # Check that the target still inherits mergeinfo from it's nearest
  # repository ancestor.
  #
  # Commit all the merges thus far
  expected_output = wc.State(wc_dir, {
    'A_COPY'           : Item(verb='Sending'),
    'A_COPY/B'         : Item(verb='Sending'),
    'A_COPY/B/E/beta'  : Item(verb='Sending'),
    'A_COPY/D'         : Item(verb='Sending'),
    'A_COPY/D/G/rho'   : Item(verb='Sending'),
    'A_COPY/D/H/omega' : Item(verb='Sending'),
    'A_COPY/D/H/psi'   : Item(verb='Sending'),
    })
  wc_status.tweak('A_COPY', 'A_COPY/B', 'A_COPY/B/E/beta', 'A_COPY/D',
                  'A_COPY/D/G/rho', 'A_COPY/D/H/omega', 'A_COPY/D/H/psi',
                  wc_rev=7)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        wc_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Copy the subtree A_COPY/B/E from the working copy, making the
  # disconnected WC E_only.
  other_wc = sbox.add_wc_path('E_only')
  svntest.actions.duplicate_dir(E_COPY_path, other_wc)

  # Update the disconnected WC it so it will get the most recent mergeinfo
  # from the repos when merging.
  svntest.actions.run_and_verify_svn(None, ["At revision 7.\n"], [], 'up',
                                     other_wc)

  # Merge r5:4 into the root of the disconnected WC.
  # E_only has no explicit mergeinfo and since it's the root of the WC
  # cannot inherit and mergeinfo from a working copy ancestor path. Nor
  # does it have any mergeinfo explicitly set on it in the repository.
  # An ancestor path on the repository side, A_COPY/B does have the merge
  # info '/A/B:1,3,5' however and E_only should inherit this, resulting in
  # mergeinfo of 'A/B/E:1,3' after the removal of r5.
  short_other_wc_path = shorten_path_kludge(other_wc)
  expected_output = wc.State(short_other_wc_path,
                             {'beta' : Item(status='U ')})
  expected_status = wc.State(short_other_wc_path, {
    ''      : Item(status=' M', wc_rev=7),
    'alpha' : Item(status='  ', wc_rev=7),
    'beta'  : Item(status='M ', wc_rev=7),
    })
  expected_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:1,3'}),
    'alpha' : Item("This is the file 'alpha'.\n"),
    'beta'  : Item("This is the file 'beta'.\n"),
    })
  expected_skip = wc.State(short_other_wc_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_other_wc_path, '5', '4',
                                       sbox.repo_url + \
                                       '/A/B/E',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)


def mergeinfo_elision(sbox):
  "mergeinfo elides to ancestor with identical info"

  # When a merge would result in mergeinfo on a target which is identical
  # to mergeinfo (local or committed) on one of the node's ancestors (the
  # nearest ancestor takes precedence), then the mergeinfo elides from the
  # target to the nearest ancestor (e.g. no mergeinfo is set on the target
  # or committed mergeinfo is removed).

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox)

  # Some paths we'll care about
  A_COPY_path      = os.path.join(wc_dir, "A_COPY")
  beta_COPY_path   = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  G_COPY_path      = os.path.join(wc_dir, "A_COPY", "D", "G")

  # Now start merging...

  # Merge r5 into A_COPY/B/E/beta.
  short_beta_COPY_path = shorten_path_kludge(beta_COPY_path)
  expected_skip = wc.State(short_beta_COPY_path, { })
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5,
                                       'U    ' + short_beta_COPY_path + '\n'),
                                     [], 'merge', '-c5',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)
  os.chdir(saved_cwd)

  # Check beta's status and props.
  expected_status = wc.State(beta_COPY_path, {
    ''        : Item(status='MM', wc_rev=2),
    })
  svntest.actions.run_and_verify_status(beta_COPY_path, expected_status)

  svntest.actions.run_and_verify_svn(None, ["/A/B/E/beta:1,5\n"], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     beta_COPY_path)

  # Commit the merge
  expected_output = wc.State(wc_dir, {
    'A_COPY/B/E/beta' : Item(verb='Sending'),
    })
  wc_status.tweak('A_COPY/B/E/beta', wc_rev=7)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        wc_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Merge r4 into A_COPY/D/G.
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_G_COPY_path = shorten_path_kludge(G_COPY_path)
  expected_output = wc.State(short_G_COPY_path, {
    'rho' : Item(status='U ')
    })
  expected_status = wc.State(short_G_COPY_path, {
    ''    : Item(status=' M', wc_rev=2),
    'pi'  : Item(status='  ', wc_rev=2),
    'rho' : Item(status='M ', wc_rev=2),
    'tau' : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''    : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G:1,4'}),
    'pi'  : Item("This is the file 'pi'.\n"),
    'rho' : Item("New content"),
    'tau' : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State(short_G_COPY_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_G_COPY_path, '3', '4',
                                       sbox.repo_url + \
                                       '/A/D/G',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Merge r3:6 into A_COPY.  This would result in identical mergeinfo
  # (r1,4-6) on A_COPY and two of it's descendents, A_COPY/D/G and
  # A_COPY/B/E/beta, so the mergeinfo on the latter two should elide
  # to A_COPY.  In the case of A_COPY/D/G this means its wholly uncommitted
  # mergeinfo is removed leaving no prop mods.  In the case of
  # A_COPY/B/E/beta its committed mergeinfo prop is removed leaving a prop
  # change.
  short_A_COPY_path = shorten_path_kludge(A_COPY_path)
  expected_output = wc.State(short_A_COPY_path, {
    'D/H/omega' : Item(status='U ')
    })
  expected_status = wc.State(short_A_COPY_path, {
    ''          : Item(status=' M', wc_rev=2),
    'B'         : Item(status='  ', wc_rev=2),
    'mu'        : Item(status='  ', wc_rev=2),
    'B/E'       : Item(status='  ', wc_rev=2),
    'B/E/alpha' : Item(status='  ', wc_rev=2),
    'B/E/beta'  : Item(status=' M', wc_rev=7),
    'B/lambda'  : Item(status='  ', wc_rev=2),
    'B/F'       : Item(status='  ', wc_rev=2),
    'C'         : Item(status='  ', wc_rev=2),
    'D'         : Item(status='  ', wc_rev=2),
    'D/G'       : Item(status='  ', wc_rev=2),
    'D/G/pi'    : Item(status='  ', wc_rev=2),
    'D/G/rho'   : Item(status='M ', wc_rev=2),
    'D/G/tau'   : Item(status='  ', wc_rev=2),
    'D/gamma'   : Item(status='  ', wc_rev=2),
    'D/H'       : Item(status='  ', wc_rev=2),
    'D/H/chi'   : Item(status='  ', wc_rev=2),
    'D/H/psi'   : Item(status='  ', wc_rev=2),
    'D/H/omega' : Item(status='M ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,4-6'}),
    'B'         : Item(),
    'mu'        : Item("This is the file 'mu'.\n"),
    'B/E'       : Item(),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("New content"),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/F'       : Item(),
    'C'         : Item(),
    'D'         : Item(),
    'D/G'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("New content"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D/H'       : Item(),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
    'D/H/psi'   : Item("This is the file 'psi'.\n"),
    'D/H/omega' : Item("New content"),
    })
  expected_skip = wc.State(short_A_COPY_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY_path, '3', '6',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Reverse merge r5 out of A_COPY/B/E/beta.  The mergeinfo on
  # A_COPY/B/E/beta which previously elided will now return,
  # minus r5 of course.
  expected_skip = wc.State(short_beta_COPY_path, { })

  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-5,
                                       'U    ' + short_beta_COPY_path + '\n'),
                                     [], 'merge', '-c-5',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)
  os.chdir(saved_cwd)

  # Check beta's status and props.
  expected_status = wc.State(beta_COPY_path, {
    ''        : Item(status='MM', wc_rev=7),
    })
  svntest.actions.run_and_verify_status(beta_COPY_path, expected_status)

  svntest.actions.run_and_verify_svn(None, ["/A/B/E/beta:1,4,6\n"], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     beta_COPY_path)

  # Merge r5 back into A_COPY/B/E/beta.  Now the mergeinfo on the merge
  # target (A_COPY/B/E/beta) is identical to it's nearest ancestor with
  # mergeinfo (A_COPY) and so the former should elide.
  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5,
                                      'G    ' + short_beta_COPY_path + '\n'),
                                     [], 'merge', '-c5',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)
  os.chdir(saved_cwd)

  # Check beta's status and props.
  expected_status = wc.State(beta_COPY_path, {
    ''        : Item(status=' M', wc_rev=7),
    })
  svntest.actions.run_and_verify_status(beta_COPY_path, expected_status)

  # Once again A_COPY/B/E/beta has no mergeinfo.
  svntest.actions.run_and_verify_svn(None, [], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     beta_COPY_path)

def mergeinfo_inheritance_and_discontinuous_ranges(sbox):
  "discontinuous merges produce correct mergeinfo"

  # When a merge target has no explicit mergeinfo and is subject
  # to multiple merges, the resulting mergeinfo on the target
  # should reflect the combination of the inherited mergeinfo
  # with each merge performed.
  #
  # Also tests implied merge source and target when only a revision
  # range is specified.

  sbox.build()
  wc_dir = sbox.wc_dir

  # Some paths we'll care about
  A_COPY_path      = os.path.join(wc_dir, "A_COPY")
  D_COPY_path      = os.path.join(wc_dir, "A_COPY", "D")
  A_COPY_rho_path  = os.path.join(wc_dir, "A_COPY", "D", "G", "rho")

  expected_disk, expected_status = setup_branch(sbox)

  # Merge r4 into A_COPY
  saved_cwd = os.getcwd()

  os.chdir(A_COPY_path)
  # Use run_and_verify_svn rather than run_and_verify_merge so we
  # can test the implied merge source functionality.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(4, 'U    ' +
                                       os.path.join("D", "G", "rho") + '\n'),
                                     [], 'merge', '-c4',
                                     '--use-merge-history')
  os.chdir(saved_cwd)

  # Check the results of the merge.
  expected_status.tweak("A_COPY", status=' M')
  expected_status.tweak("A_COPY/D/G/rho", status='M ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  svntest.actions.run_and_verify_svn(None, ["/A:1,4\n"], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     A_COPY_path)

  # Merge r2:6 into A_COPY/D
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  #
  # A_COPY/D should inherit the mergeinfo '/A:1,4' from A_COPY
  # combine it with the discontinous merges performed directly on
  # it (A/D/ 2:3 and A/D 4:6) resulting in '/A/D:1,3-6'.
  short_D_COPY_path = shorten_path_kludge(D_COPY_path)
  expected_output = wc.State(short_D_COPY_path, {
    'H/psi'   : Item(status='U '),
    'H/omega' : Item(status='U '),
    })
  expected_status = wc.State(short_D_COPY_path, {
    ''        : Item(status=' M', wc_rev=2),
    'G'       : Item(status='  ', wc_rev=2),
    'G/pi'    : Item(status='  ', wc_rev=2),
    'G/rho'   : Item(status='M ', wc_rev=2),
    'G/tau'   : Item(status='  ', wc_rev=2),
    'H'       : Item(status='  ', wc_rev=2),
    'H/chi'   : Item(status='  ', wc_rev=2),
    'H/psi'   : Item(status='M ', wc_rev=2),
    'H/omega' : Item(status='M ', wc_rev=2),
    'gamma'   : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1,3-6'}),
    'G'       : Item(),
    'G/pi'    : Item("This is the file 'pi'.\n"),
    'G/rho'   : Item("New content"),
    'G/tau'   : Item("This is the file 'tau'.\n"),
    'H'       : Item(),
    'H/chi'   : Item("This is the file 'chi'.\n"),
    'H/psi'   : Item("New content"),
    'H/omega' : Item("New content"),
    'gamma'   : Item("This is the file 'gamma'.\n")
    })
  expected_skip = wc.State(short_D_COPY_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '2', '6',
                                       sbox.repo_url + '/A/D',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Wipe the memory of a portion of the previous merge...
  ### It'd be nice to use 'merge --record-only' here, but we can't (yet)
  ### wipe all ranges for a file due to the bug pointed out in r24645.
  mu_copy_path = os.path.join(A_COPY_path, 'mu')
  svntest.actions.run_and_verify_svn(None,
                                     ["property '" + SVN_PROP_MERGE_INFO
                                      + "' set on '" +
                                      mu_copy_path + "'\n"], [], 'propset',
                                     SVN_PROP_MERGE_INFO, '', mu_copy_path)
  # ...and confirm that we can commit the wiped mergeinfo...
  expected_output = wc.State(wc_dir, {
    'A_COPY/mu' : Item(verb='Sending'),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        None,
                                        None,
                                        None, None, None, None,
                                        mu_copy_path)
  # ...and that the presence of the property is retained, even when
  # the value has been wiped.
  svntest.actions.run_and_verify_svn(None, ['\n'], [], 'propget',
                                     SVN_PROP_MERGE_INFO, mu_copy_path)

def merge_to_target_with_copied_children(sbox):
  "merge works when target has copied children"

  # Test for Issue #2754 Can't merge to target with copied/moved children

  sbox.build()
  wc_dir = sbox.wc_dir
  expected_disk, expected_status = setup_branch(sbox)

  # Some paths we'll care about
  D_COPY_path = os.path.join(wc_dir, "A_COPY", "D")
  G_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G")

  # URL to URL copy A_COPY/D/G/rho to A_COPY/D/G/rho_copy
  svntest.actions.run_and_verify_svn(None, None, [], 'copy',
                                     sbox.repo_url + '/A_COPY/D/G/rho',
                                     sbox.repo_url + '/A_COPY/D/G/rho_copy',
                                     '-m', 'copy')

  # Update WC.
  expected_output = wc.State(wc_dir,
                             {'A_COPY/D/G/rho_copy' : Item(status='A ')})
  expected_disk.add({
    'A_COPY/D/G/rho_copy' : Item("This is the file 'rho'.\n",
                                 props={SVN_PROP_MERGE_INFO :
                                        '/A/D/G/rho:1\n/A_COPY/D/G/rho:2-6\n'})
    })
  expected_status.tweak(wc_rev=7)
  expected_status.add({'A_COPY/D/G/rho_copy' : Item(status='  ', wc_rev=7)})
  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None,
                                        None, None, 1)

  # Merge r4 into A_COPY/D/G.
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_G_COPY_path = shorten_path_kludge(G_COPY_path)
  expected_output = wc.State(short_G_COPY_path, {
    'rho' : Item(status='U ')
    })
  expected_status = wc.State(short_G_COPY_path, {
    ''         : Item(status=' M', wc_rev=7),
    'pi'       : Item(status='  ', wc_rev=7),
    'rho'      : Item(status='M ', wc_rev=7),
    'rho_copy' : Item(status='  ', wc_rev=7),
    'tau'      : Item(status='  ', wc_rev=7),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G:1,4'}),
    'pi'       : Item("This is the file 'pi'.\n"),
    'rho'      : Item("New content"),
    'rho_copy' : Item("This is the file 'rho'.\n",
                      props={SVN_PROP_MERGE_INFO :
                             '/A/D/G/rho:1\n/A_COPY/D/G/rho:2-6\n'}),
    'tau'      : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State(short_G_COPY_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_G_COPY_path, '3', '4',
                                       sbox.repo_url + \
                                       '/A/D/G',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)

def merge_to_switched_path(sbox):
  "merge to switched path does not inherit or elide"

  # When the target of a merge is a switched path we don't inherit WC
  # mergeinfo from above the target or attempt to elide the mergeinfo
  # set on the target as a result of the merge.

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox)

  # Some paths we'll care about
  A_COPY_D_path = os.path.join(wc_dir, "A_COPY", "D")
  G_COPY_path = os.path.join(wc_dir, "A", "D", "G_COPY")
  A_COPY_D_G_path = os.path.join(wc_dir, "A_COPY", "D", "G")
  A_COPY_D_G_rho_path = os.path.join(wc_dir, "A_COPY", "D", "G", "rho")

  expected = svntest.verify.UnorderedOutput(
         ["A    " + os.path.join(G_COPY_path, "pi") + "\n",
          "A    " + os.path.join(G_COPY_path, "rho") + "\n",
          "A    " + os.path.join(G_COPY_path, "tau") + "\n",
          "Checked out revision 6.\n",
          "A         " + G_COPY_path + "\n"])

  # r7 - Copy A/D/G to A/D/G_COPY and commit.
  svntest.actions.run_and_verify_svn(None, expected, [], 'copy',
                                     sbox.repo_url + "/A/D/G",
                                     G_COPY_path)

  expected_output = wc.State(wc_dir, {'A/D/G_COPY' : Item(verb='Adding')})
  wc_status.add({
    "A/D/G_COPY"     : Item(status='  ', wc_rev=7),
    "A/D/G_COPY/pi"  : Item(status='  ', wc_rev=7),
    "A/D/G_COPY/rho" : Item(status='  ', wc_rev=7),
    "A/D/G_COPY/tau" : Item(status='  ', wc_rev=7),
    })

  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # r8 - modify and commit A/D/G_COPY/rho
  svntest.main.file_write(os.path.join(wc_dir, "A", "D", "G_COPY", "rho"),
                          "New *and* improved rho content")
  expected_output = wc.State(wc_dir, {'A/D/G_COPY/rho' : Item(verb='Sending')})
  wc_status.tweak('A/D/G_COPY/rho', wc_rev=8)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Switch A_COPY/D/G to A/D/G.
  wc_disk.add({
    "A"  : Item(),
    "A/D/G_COPY"     : Item(props={SVN_PROP_MERGE_INFO :
                                   '/A/D/G:1-6'}),
    "A/D/G_COPY/pi"  : Item("This is the file 'pi'.\n"),
    "A/D/G_COPY/rho" : Item("New *and* improved rho content"),
    "A/D/G_COPY/tau" : Item("This is the file 'tau'.\n"),
    })
  wc_disk.tweak('A_COPY/D/G/rho',contents="New content")
  wc_status.tweak("A_COPY/D/G", wc_rev=8, switched='S')
  wc_status.tweak("A_COPY/D/G/pi", wc_rev=8)
  wc_status.tweak("A_COPY/D/G/rho", wc_rev=8)
  wc_status.tweak("A_COPY/D/G/tau", wc_rev=8)
  expected_output = svntest.wc.State(sbox.wc_dir, {
    "A_COPY/D/G/rho"         : Item(status='U '),
    })
  svntest.actions.run_and_verify_switch(sbox.wc_dir, A_COPY_D_G_path,
                                        sbox.repo_url + "/A/D/G",
                                        expected_output, wc_disk, wc_status,
                                        None, None, None, None, None, 1)

  # Merge r8 from A/D/G_COPY into our switched target A_COPY/D/G.
  # A_COPY/D/G should get mergeinfo for r8 as a result of the merge,
  # but because it's switched should not inherit the mergeinfo from
  # its nearest WC ancestor with mergeinfo (A_COPY: svn:mergeinfo : /A:1)
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_G_COPY_path = shorten_path_kludge(A_COPY_D_G_path)
  expected_output = wc.State(short_G_COPY_path, {
    'rho' : Item(status='U ')
    })
  # Note: A_COPY/D/G won't show as switched because of the Merge Kluge.
  expected_status = wc.State(short_G_COPY_path, {
    ''         : Item(status=' M', wc_rev=8),
    'pi'       : Item(status='  ', wc_rev=8),
    'rho'      : Item(status='M ', wc_rev=8),
    'tau'      : Item(status='  ', wc_rev=8),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G_COPY:8'}),
    'pi'       : Item("This is the file 'pi'.\n"),
    'rho'      : Item("New *and* improved rho content"),
    'tau'      : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State(short_G_COPY_path, { })
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_G_COPY_path, '7', '8',
                                       sbox.repo_url + '/A/D/G_COPY',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Check that the mergeinfo set on a target doesn't elide when that
  # target is switched.
  #
  # Revert the previous merge and manually set 'svn:mergeinfo : /A/D/:'
  # on 'merge_tests-1\A_COPY\D'.  Now merge -c-4 from /A/D/G into A_COPY/D/G.
  # This should still set 'svn:mergeinfo : /A/D/G:' on
  # 'merge_tests-1\A_COPY\D\G'.  This would normally elide to A_COPY/D,
  # but since A_COPY/D/G is switched it should not.
  svntest.actions.run_and_verify_svn(None,
                                     ["Reverted '" + A_COPY_D_G_path+ "'\n",
                                      "Reverted '" + A_COPY_D_G_rho_path +
                                      "'\n"],
                                     [], 'revert', '-R', wc_dir)
  svntest.actions.run_and_verify_svn(None,
                                     ["property '" + SVN_PROP_MERGE_INFO +
                                      "' set on '" + A_COPY_D_path+ "'" +
                                      "\n"], [], 'ps', SVN_PROP_MERGE_INFO,
                                     '/A/D:', A_COPY_D_path)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-4,
                                       'U    ' + A_COPY_D_G_rho_path + '\n'),
                                     [], 'merge', '-c-4',
                                     sbox.repo_url + '/A/D/G_COPY',
                                     A_COPY_D_G_path)
  wc_status.tweak("A_COPY/D", status=' M')
  wc_status.tweak("A_COPY/D/G", status=' M')
  wc_status.tweak("A_COPY/D/G/rho", status='M ')
  svntest.actions.run_and_verify_status(wc_dir, wc_status)
  expected = svntest.verify.UnorderedOutput(
    ["A    " + os.path.join(G_COPY_path, "pi") + "\n",
     "A    " + os.path.join(G_COPY_path, "rho") + "\n",
     "A    " + os.path.join(G_COPY_path, "tau") + "\n",
     "Checked out revision 6.\n",
     "A         " + G_COPY_path + "\n"])
  expected = svntest.verify.UnorderedOutput(
    ["Properties on '" + A_COPY_D_path + "':\n",
     "  " + SVN_PROP_MERGE_INFO + " : /A/D:\n",
     "Properties on '" + A_COPY_D_G_path + "':\n",
     "  " + SVN_PROP_MERGE_INFO +" : /A/D/G:\n"])
  svntest.actions.run_and_verify_svn(None,
                                     expected, [],
                                     'pl', '-vR', A_COPY_D_path)

# Test for issues
#
#   2823: Account for mergeinfo differences for switched
#         directories when gathering mergeinfo
#
#   2839: Support non-inheritable mergeinfo revision ranges
def merge_to_path_with_switched_children(sbox):
  "merge to path with switched children"

  # Merging to a target with switched children requires special handling
  # to keep mergeinfo correct:
  #
  #   1) If the target of a merge has switched children without explicit
  #      mergeinfo, the switched children should get mergeinfo set on
  #      them as a result of the merge.  This mergeinfo includes the
  #      mergeinfo resulting from the merge *and* any mergeinfo inherited
  #      from the repos for the switched path.
  #
  #   2) Mergeinfo on switched children should never elide.
  #
  #   3) The path the switched child overrides cannot be modified by the
  #      merge (it isn't present in the WC) so should not inherit any
  #      mergeinfo added as a result of the merge.  To prevent this, the
  #      immediate parent of any switched child should have non-inheritable
  #      mergeinfo added/modified for the merge performed.
  #
  #   4) Because of 3, siblings of switched children will not inherit the
  #      mergeinfo resulting from the merge, so must get their own, full set
  #      of mergeinfo.

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, False, 3)

  # Some paths we'll care about
  D_path = os.path.join(wc_dir, "A", "D")
  A_COPY_path = os.path.join(wc_dir, "A_COPY")
  A_COPY_beta_path = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  A_COPY_chi_path = os.path.join(wc_dir, "A_COPY", "D", "H", "chi")
  A_COPY_omega_path = os.path.join(wc_dir, "A_COPY", "D", "H", "omega")
  A_COPY_psi_path = os.path.join(wc_dir, "A_COPY", "D", "H", "psi")
  A_COPY_G_path = os.path.join(wc_dir, "A_COPY", "D", "G")
  A_COPY_rho_path = os.path.join(wc_dir, "A_COPY", "D", "G", "rho")
  A_COPY_H_path = os.path.join(wc_dir, "A_COPY", "D", "H")
  A_COPY_D_path = os.path.join(wc_dir, "A_COPY", "D")
  A_COPY_gamma_path = os.path.join(wc_dir, "A_COPY", "D", "gamma")
  H_COPY_2_path = os.path.join(wc_dir, "A_COPY_2", "D", "H")

  svntest.actions.run_and_verify_svn(None, ["At revision 8.\n"], [], 'up',
                                     wc_dir)
  wc_status.tweak(wc_rev=8)

  # Switch a file and dir path in the branch:

  # Switch A_COPY/D/G to A_COPY_2/D/G.
  wc_status.tweak("A_COPY/D/G", switched='S')
  expected_output = svntest.wc.State(sbox.wc_dir, {})
  svntest.actions.run_and_verify_switch(sbox.wc_dir, A_COPY_G_path,
                                        sbox.repo_url + "/A_COPY_2/D/G",
                                        expected_output, wc_disk, wc_status,
                                        None, None, None, None, None, 1)

  # Switch A_COPY/D/G/rho to A_COPY_3/D/G/rho.
  wc_status.tweak("A_COPY/D/G/rho", switched='S')
  expected_output = svntest.wc.State(sbox.wc_dir, {})
  svntest.actions.run_and_verify_switch(sbox.wc_dir, A_COPY_rho_path,
                                        sbox.repo_url + "/A_COPY_3/D/G/rho",
                                        expected_output, wc_disk, wc_status,
                                        None, None, None, None, None, 1)

  # Switch A_COPY/D/H/psi to A_COPY_2/D/H/psi.
  wc_status.tweak("A_COPY/D/H/psi", switched='S')
  expected_output = svntest.wc.State(sbox.wc_dir, {})
  svntest.actions.run_and_verify_switch(sbox.wc_dir, A_COPY_psi_path,
                                        sbox.repo_url + "/A_COPY_2/D/H/psi",
                                        expected_output, wc_disk, wc_status,
                                        None, None, None, None, None, 1)

  # Target with switched file child:
  #
  # Merge r8 from A/D/H into A_COPY/D/H.  The switched child of
  # A_COPY/D/H, file A_COPY/D/H/psi (which has no mergeinfo prior
  # to the merge), should get their own mergeinfo, both r8 from the
  # merge itself, and r1-2 inherited from A_COPY_2 in the repository.
  #
  # A_COPY/D/H/psi's parent A_COPY/D/H has no pre-exiting explicit
  # mergeinfo so should get its own mergeinfo, made up of r1 inherited
  # from A_COPY and the non-inheritable r8 resulting from the merge.
  #
  # A_COPY/D/H/psi's two unswitched siblings, A_COPY/D/H/chi and
  # A_COPY/D/H/omega won't inherit r8 from A_COPY/D/H, so need their
  # own mergeinfo.
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_H_COPY_path = shorten_path_kludge(A_COPY_H_path)
  expected_output = wc.State(short_H_COPY_path, {
    'omega' : Item(status='U ')
    })
  expected_status = wc.State(short_H_COPY_path, {
    ''      : Item(status=' M', wc_rev=8),
    'psi'   : Item(status=' M', wc_rev=8, switched='S'),
    'omega' : Item(status='MM', wc_rev=8),
    'chi'   : Item(status=' M', wc_rev=8),
    })
  expected_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,8*'}),
    'psi'   : Item("This is the file 'psi'.\n",
                   props={SVN_PROP_MERGE_INFO : '/A/D/H/psi:1-2,8'}),
    'omega' : Item("New content",
                   props={SVN_PROP_MERGE_INFO : '/A/D/H/omega:1,8'}),
    'chi'   : Item("This is the file 'chi'.\n",
                   props={SVN_PROP_MERGE_INFO : '/A/D/H/chi:1,8'}),
    })
  expected_skip = wc.State(short_H_COPY_path, { })
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)

  svntest.actions.run_and_verify_merge(short_H_COPY_path, '7', '8',
                                       sbox.repo_url + '/A/D/H',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Target with switched dir child:
  #
  # Merge r6 from A/D into A_COPY/D.  The switched child of A_COPY/D,
  # directory A_COPY/D/G (which has no mergeinfo prior to the merge),
  # should get its own mergeinfo, both for r6 from the merge itself and
  # r1-2 from the repository.
  #
  # A_COPY/D/G's parent A_COPY/D has no pre-exiting explicit
  # mergeinfo so should get its own mergeinfo, made up of r1 inherited
  # from A_COPY and the non-inheritable r5 resulting from the merge.
  #
  # A_COPY/D/G's two unswitched siblings, A_COPY/D/gamma and A_COPY/D/H
  # won't inherit r5 from A_COPY/D, so need their own mergeinfo
  # added/updated respectively.  A_COPY/D/H itself has a switched child
  # so r5 is set as non-inheritable on it.  All of A_COPY/D/H's children,
  # which have explict mergeinfo from the previous merge, get r5 in their
  # mergeinfo.
  short_D_COPY_path = shorten_path_kludge(A_COPY_D_path)
  expected_output = wc.State(short_D_COPY_path, {
    'G/rho' : Item(status='U ')
    })
  expected_status_D = wc.State(short_D_COPY_path, {
    ''        : Item(status=' M', wc_rev=8),
    'H'       : Item(status=' M', wc_rev=8),
    'H/chi'   : Item(status=' M', wc_rev=8),
    'H/omega' : Item(status='MM', wc_rev=8),
    'H/psi'   : Item(status=' M', wc_rev=8, switched='S'),
    'G'       : Item(status=' M', wc_rev=8, switched='S'),
    'G/pi'    : Item(status=' M', wc_rev=8),
    'G/rho'   : Item(status='MM', wc_rev=8, switched='S'),
    'G/tau'   : Item(status=' M', wc_rev=8),
    'gamma'   : Item(status=' M', wc_rev=8),
    })
  expected_disk_D = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1,6*'}),
    'H'       : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,6*,8*'}),
    'H/chi'   : Item("This is the file 'chi'.\n",
                     props={SVN_PROP_MERGE_INFO : '/A/D/H/chi:1,6,8'}),
    'H/omega' : Item("New content",
                     props={SVN_PROP_MERGE_INFO : '/A/D/H/omega:1,6,8'}),
    'H/psi'   : Item("This is the file 'psi'.\n",
                     props={SVN_PROP_MERGE_INFO : '/A/D/H/psi:1-2,6,8'}),
    'G'       : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G:1-2,6*'}),
    'G/pi'    : Item("This is the file 'pi'.\n",
                     props={SVN_PROP_MERGE_INFO : '/A/D/G/pi:1-2,6'}),
    'G/rho'   : Item("New content",
                     props={SVN_PROP_MERGE_INFO : '/A/D/G/rho:1-3,6'}),
    'G/tau'   : Item("This is the file 'tau'.\n",
                     props={SVN_PROP_MERGE_INFO : '/A/D/G/tau:1-2,6'}),
    'gamma'   : Item("This is the file 'gamma'.\n",
                     props={SVN_PROP_MERGE_INFO : '/A/D/gamma:1,6'}),
    })
  expected_skip_D = wc.State(short_D_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '5', '6',
                                       sbox.repo_url + '/A/D',
                                       expected_output, expected_disk_D,
                                       expected_status_D, expected_skip_D,
                                       None, None, None, None, None, 1)

  # Merge r5 from A/D into A_COPY/D.  A_COPY/D's switched child A_COPY/D/G
  # gets r5 as does A_COPY/D/G's unswitched sibling A_COPY/D/gamma.  Since
  # A_COPY/D has a switched child, it gets an uninheritable r5.  Every other
  # path with existing mergeinfo already has r8 so are unchanged.
  #
  # A_COPY/D/H/chi and A_COPY/D/H/omega both have mergeinfo for r1,6,8 but
  # won't elide to A_COPY/D/H becuase the latter's mergeinfo, while
  # encompassing the same ranges, r1,6*,8*, has some non-inheritable ranges.
  # The same is true of A_COPY/D/gamma and A_COPY/D.
  expected_output = wc.State(short_D_COPY_path, {
    'H/psi' : Item(status='U ')})
  expected_disk_D.tweak('', props={SVN_PROP_MERGE_INFO : '/A/D:1,5-6*'})
  expected_disk_D.tweak('G', props={SVN_PROP_MERGE_INFO : '/A/D/G:1-2,5-6*'})
  expected_disk_D.tweak('G/pi',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G/pi:1-2,5-6'})
  expected_disk_D.tweak('G/rho',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G/rho:1-3,5-6'})
  expected_disk_D.tweak('G/tau',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G/tau:1-2,5-6'})
  expected_disk_D.tweak('H', props={SVN_PROP_MERGE_INFO : '/A/D/H:1,5-6*,8*'})
  expected_disk_D.tweak('gamma',
                        props={SVN_PROP_MERGE_INFO : '/A/D/gamma:1,5-6'})
  expected_disk_D.tweak('H/chi',
                        props={SVN_PROP_MERGE_INFO :'/A/D/H/chi:1,5-6,8'})
  expected_disk_D.tweak('H/psi', contents="New content",
                        props={SVN_PROP_MERGE_INFO :'/A/D/H/psi:1-2,5-6,8'})
  expected_disk_D.tweak('H/omega',
                        props={SVN_PROP_MERGE_INFO :'/A/D/H/omega:1,5-6,8'})
  expected_status_D.tweak('H/psi', status='MM')
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '4', '5',
                                       sbox.repo_url + '/A/D',
                                       expected_output, expected_disk_D,
                                       expected_status_D, expected_skip_D,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Finally, merge r4:8 into A_COPY.  A_COPY gets r5-8 added and every path
  # under it with with explicit mergeinfo gets r5,7 added (they all have r6,8
  # already).  Again there is no elision, since the only possibilities, e.g.
  # A_COPY/D/H's '/A/D/H:1,5-8*' to A_COPY/D's '/A/D:1,5-8*', involve
  # non-inheritable mergeinfo one ond side or the other.
  short_A_COPY_path = shorten_path_kludge(A_COPY_path)
  expected_output = wc.State(short_A_COPY_path, {
    'B/E/beta' : Item(status='U ')
    })
  expected_status = wc.State(short_A_COPY_path, {
    ''          : Item(status=' M', wc_rev=8),
    'B'         : Item(status='  ', wc_rev=8),
    'mu'        : Item(status='  ', wc_rev=8),
    'B/E'       : Item(status='  ', wc_rev=8),
    'B/E/alpha' : Item(status='  ', wc_rev=8),
    'B/E/beta'  : Item(status='M ', wc_rev=8),
    'B/lambda'  : Item(status='  ', wc_rev=8),
    'B/F'       : Item(status='  ', wc_rev=8),
    'C'         : Item(status='  ', wc_rev=8),
    'D'         : Item(status=' M', wc_rev=8),
    'D/G'       : Item(status=' M', wc_rev=8, switched='S'),
    'D/G/pi'    : Item(status=' M', wc_rev=8),
    'D/G/rho'   : Item(status='MM', wc_rev=8, switched='S'),
    'D/G/tau'   : Item(status=' M', wc_rev=8),
    'D/gamma'   : Item(status=' M', wc_rev=8),
    'D/H'       : Item(status=' M', wc_rev=8),
    'D/H/chi'   : Item(status=' M', wc_rev=8),
    'D/H/psi'   : Item(status='MM', wc_rev=8, switched='S'),
    'D/H/omega' : Item(status='MM', wc_rev=8),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,5-8'}),
    'B'         : Item(),
    'mu'        : Item("This is the file 'mu'.\n"),
    'B/E'       : Item(),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("New content"),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/F'       : Item(),
    'C'         : Item(),
    'D'         : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1,5-8*'}),
    'D/G'       : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G:1-2,5-8*'}),
    'D/G/pi'    : Item("This is the file 'pi'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/D/G/pi:1-2,5-8'}),
    'D/G/rho'   : Item("New content",
                       props={SVN_PROP_MERGE_INFO : '/A/D/G/rho:1-3,5-8'}),
    'D/G/tau'   : Item("This is the file 'tau'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/D/G/tau:1-2,5-8'}),
    'D/gamma'   : Item("This is the file 'gamma'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/D/gamma:1,5-8'}),
    'D/H'       : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,5-8*'}),
    'D/H/chi'   : Item("This is the file 'chi'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/D/H/chi:1,5-8'}),
    'D/H/psi'   : Item("New content",
                       props={SVN_PROP_MERGE_INFO : '/A/D/H/psi:1-2,5-8'}),
    'D/H/omega' : Item("New content",
                       props={SVN_PROP_MERGE_INFO : '/A/D/H/omega:1,5-8'}),
    })
  expected_skip = wc.State(short_A_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY_path, '4', '8',
                                       sbox.repo_url + '/A',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)

  os.chdir(saved_cwd)

  # Commit changes thus far.
  expected_output = svntest.wc.State(wc_dir, {
    'A_COPY'           : Item(verb='Sending'),
    'A_COPY/B/E/beta'  : Item(verb='Sending'),
    'A_COPY/D'         : Item(verb='Sending'),
    'A_COPY/D/gamma'   : Item(verb='Sending'),
    'A_COPY/D/G'       : Item(verb='Sending'),
    'A_COPY/D/G/pi'    : Item(verb='Sending'),
    'A_COPY/D/G/rho'   : Item(verb='Sending'),
    'A_COPY/D/G/tau'   : Item(verb='Sending'),
    'A_COPY/D/H'         : Item(verb='Sending'),
    'A_COPY/D/H/chi'     : Item(verb='Sending'),
    'A_COPY/D/H/omega'   : Item(verb='Sending'),
    'A_COPY/D/H/psi'     : Item(verb='Sending'),
    })
  wc_status.tweak('A_COPY', 'A_COPY/B/E/beta', 'A_COPY/D', 'A_COPY/D/gamma',
                  'A_COPY/D/G', 'A_COPY/D/G/pi', 'A_COPY/D/G/rho',
                  'A_COPY/D/G/tau','A_COPY/D/H', 'A_COPY/D/H/chi',
                  'A_COPY/D/H/omega', 'A_COPY/D/H/psi',
                  wc_rev=9)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)

  # Unswitch A_COPY/D/H/psi.
  expected_output = svntest.wc.State(wc_dir, {
    'A_COPY/D/H/psi' : Item(status='UU')})
  wc_status.tweak("A_COPY/D/H/psi", switched=None, wc_rev=9)
  wc_disk.tweak("A_COPY",
                props={SVN_PROP_MERGE_INFO : '/A:1,5-8'})
  wc_disk.tweak("A_COPY/B/E/beta",
                contents="New content")
  wc_disk.tweak("A_COPY/D",
                props={SVN_PROP_MERGE_INFO : '/A/D:1,5-8*'})
  wc_disk.tweak("A_COPY/D/gamma",
                props={SVN_PROP_MERGE_INFO : '/A/D/gamma:1,5-8'})
  wc_disk.tweak("A_COPY/D/G",
                props={SVN_PROP_MERGE_INFO : '/A/D/G:1-2,5-8*'})
  wc_disk.tweak("A_COPY/D/G/pi",
                props={SVN_PROP_MERGE_INFO : '/A/D/G/pi:1-2,5-8'})
  wc_disk.tweak("A_COPY/D/G/rho",
                contents="New content",
                props={SVN_PROP_MERGE_INFO : '/A/D/G/rho:1-3,5-8'})
  wc_disk.tweak("A_COPY/D/G/tau",
                props={SVN_PROP_MERGE_INFO : '/A/D/G/tau:1-2,5-8'})
  wc_disk.tweak("A_COPY/D/H",
                props={SVN_PROP_MERGE_INFO : '/A/D/H:1,5-8*'})
  wc_disk.tweak("A_COPY/D/H/chi",
                props={SVN_PROP_MERGE_INFO : '/A/D/H/chi:1,5-8'})
  wc_disk.tweak("A_COPY/D/H/omega",
                contents="New content",
                props={SVN_PROP_MERGE_INFO : '/A/D/H/omega:1,5-8'})
  wc_disk.tweak("A_COPY_2",
                props={SVN_PROP_MERGE_INFO : '/A:1-2'})
  svntest.actions.run_and_verify_switch(sbox.wc_dir, A_COPY_psi_path,
                                        sbox.repo_url + "/A_COPY/D/H/psi",
                                        expected_output, wc_disk, wc_status,
                                        None, None, None, None, None, 1)

  # Non-inheritable mergeinfo ranges on a target don't prevent repeat
  # merges of that range on the target's children.
  #
  # Non-inheritable mergeinfo ranges on a target are removed if the target
  # no longer has any switched children and a repeat merge is performed.
  #
  # Merge r4:8 from A/D/H into A_COPY/D/H.  A_COPY/D/H already has mergeinfo
  # for r4:8 but it is marked as uninheritable so the repeat merge is
  # allowed on its children, notably the now unswitched A_COPY/D/H/psi.
  # Since A_COPY/D/H no longer has any switched children and the merge of
  # r4:8 has been repeated the previously uninheritable ranges 5-8* on
  # A_COPY/D/H are made inheritable.  This also means that the mergeinfo on
  # A_COPY/D/H's children will elide to it.
  short_H_COPY_path = shorten_path_kludge(A_COPY_H_path)
  expected_output = wc.State(short_H_COPY_path, {
    'psi' : Item(status='U ')
    })
  expected_status = wc.State(short_H_COPY_path, {
    ''      : Item(status=' M', wc_rev=9),
    'psi'   : Item(status='M ', wc_rev=9),
    'omega' : Item(status=' M', wc_rev=9),
    'chi'   : Item(status=' M', wc_rev=9),
    })
  expected_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,5-8'}),
    'psi'   : Item("New content"),
    'omega' : Item("New content"),
    'chi'   : Item("This is the file 'chi'.\n"),
    })
  expected_skip = wc.State(short_H_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_H_COPY_path, '4', '8',
                                       sbox.repo_url + '/A/D/H',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Non-inheritable mergeinfo ranges on a target do prevent repeat
  # merges on the target itself.
  #
  # Add a prop A/D and commit it as r10.  Merge r10 into A_COPY/D.  Since
  # A_COPY/D has a switched child it gets r10 added as a non-inheritable
  # range.  Repeat the same merge checking that no repeat merge is
  # attempted on A_COPY/D.
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      D_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', D_path)
  expected_output = svntest.wc.State(wc_dir, {
    'A/D'            : Item(verb='Sending'),
    'A_COPY/D/H'       : Item(verb='Sending'),
    'A_COPY/D/H/chi'   : Item(verb='Sending'),
    'A_COPY/D/H/psi'   : Item(verb='Sending'),
    'A_COPY/D/H/omega' : Item(verb='Sending'),
    })
  wc_status.tweak('A_COPY/D', wc_rev=9)
  wc_status.tweak('A/D', 'A_COPY/D/H', 'A_COPY/D/H/chi', 'A_COPY/D/H/psi',
                  'A_COPY/D/H/omega', wc_rev=10)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)
  expected_output = wc.State(short_D_COPY_path, {
    '' : Item(status=' U')
    })
  # Reuse expected status and disk from last merge to A_COPY/D
  expected_status_D.tweak('', 'gamma', status=' M', wc_rev=9)
  expected_status_D.tweak('H', status=' M', wc_rev=10)
  expected_status_D.tweak('H/psi', 'H/chi', 'H/omega', status='  ', wc_rev=10,
                          switched=None)
  expected_status_D.tweak('G', switched='S', status=' M', wc_rev=9)
  expected_status_D.tweak('G/tau', 'G/pi', 'G/rho', status=' M', wc_rev=9)
  expected_disk_D.tweak('', props={SVN_PROP_MERGE_INFO : '/A/D:1,5-8*,10*',
                                   "prop:name" : "propval"})
  expected_disk_D.tweak('G',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G:1-2,5-8*,10*'})
  expected_disk_D.tweak('G/pi',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G/pi:1-2,5-8,10'})
  expected_disk_D.tweak('G/rho',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G/rho:1-3,5-8,10'})
  expected_disk_D.tweak('G/tau',
                        props={SVN_PROP_MERGE_INFO : '/A/D/G/tau:1-2,5-8,10'})
  expected_disk_D.tweak('H', props={SVN_PROP_MERGE_INFO : '/A/D/H:1,5-8,10'})
  expected_disk_D.tweak('gamma',
                        props={SVN_PROP_MERGE_INFO : '/A/D/gamma:1,5-8,10'})
  expected_disk_D.tweak('H/chi', 'H/omega', props={})
  expected_disk_D.tweak('H/psi', contents="New content", props={})
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '9', '10',
                                       sbox.repo_url + '/A/D',
                                       expected_output, expected_disk_D,
                                       expected_status_D, expected_skip_D,
                                       None, None, None, None, None, 1)
  # Repeated merge is a no-op.
  expected_output = wc.State(short_D_COPY_path, {})
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '9', '10',
                                       sbox.repo_url + '/A/D',
                                       expected_output, expected_disk_D,
                                       expected_status_D, expected_skip_D,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)


# Test for issue 2047: Merge from parent dir fails while it succeeds from
# the direct dir
def merge_with_implicit_target_file(sbox):
  "merge a change to a file, using relative path"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a change to A/mu, then revert it using 'svn merge -r 2:1 A/mu'

  # change A/mu and commit
  A_path = os.path.join(wc_dir, 'A')
  mu_path = os.path.join(A_path, 'mu')

  svntest.main.file_append(mu_path, "A whole new line.\n")

  expected_output = svntest.wc.State(wc_dir, {
    'A/mu' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', wc_rev=2)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Update to revision 2.
  svntest.actions.run_and_verify_svn(None, None, [], 'update', wc_dir)

  # Revert the change committed in r2
  os.chdir(wc_dir)

  # run_and_verify_merge doesn't accept file paths.
  svntest.actions.run_and_verify_svn(None, None, [], 'merge', '-r', '2:1',
                                     'A/mu')

# Test practical application of issue #2769 fix, empty rev range elision,
# and elision to the repos.
def empty_rev_range_mergeinfo(sbox):
  "mergeinfo can have empty rev ranges"

  # This test covers three areas:
  #
  # 1) The fix for issue #2769 which permits mergeinfo with empty range
  #    lists.  This allows mergeinfo on a path to override mergeinfo the
  #    path would otherwise inherit from an ancestor.  e.g. Merging -rX:Y
  #    from URL/SOURCE to PATH then merging -rY:X from URL/SOURCE to PATH's
  #    child PATH_C should result in mergeinfo for SOURCE with an empty
  #    rangelist on PATH_C
  #
  # 2) Elision of mergeinfo where some or all paths in either the child or
  #    parent's mergeinfo map to empty revision ranges.  This takes many
  #    forms -- Where C is the path to be elided, MC is it's mergeinfo, P is
  #    C's nearest ancestor with mergeinfo, and MP is P's mergeinfo:
  #
  #      a) In terms of elision, empty revision ranges are considered
  #         equivalent to each other just like any other revision range.
  #
  #      b) If MC consists only of paths mapped to empty revision ranges and
  #         none of these paths exist in MP, then MC fully elides.
  #
  #      c) If MC is equivalent to MP except for paths existing only in MC
  #         which map to empty revision ranges, then MC fully elides.
  #
  #      d) If MC is equivalent to MP except for paths existing only in MP
  #         which map to empty revision ranges, then MC fully elides.
  #
  #      e) Similar to d: MC contains some paths mapped to empty revision
  #         ranges and these paths don't exist in MP, but the remaining info
  #         in MC is *NOT* equivalent to MP, then MC partially elides, that is
  #         only the paths mapped to empty ranges for paths unique to MC elide.
  #
  #      f) If MC consists *only* of paths mapped to empty revision ranges, and
  #         has no ancestor with mergeinfo, MC still "elides".
  #
  # 3) A path with mergeinfo which has no working copy ancestor with
  #    mergeinfo may still elide to an ancestor with equivalent mergeinfo
  #    in the repository.

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True)

  # Some paths we'll care about
  A_path = os.path.join(wc_dir, "A")
  A_COPY_path = os.path.join(wc_dir, "A_COPY")
  A_D_path = os.path.join(wc_dir, "A", "D")
  A_D_H_path = os.path.join(wc_dir, "A", "D", "H")
  A_COPY_beta_path = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  rho_path = os.path.join(wc_dir, "A", "D", "G", "rho")
  psi_path = os.path.join(wc_dir, "A", "D", "H", "psi")
  gamma_path = os.path.join(wc_dir, "A", "D", "gamma")

  # Make some changes in the branch to merge back to A:

  # r3 - modify and commit A_COPY/D/H/psi
  svntest.main.file_write(os.path.join(wc_dir, "A_COPY", "D", "H", "psi"),
                          "New content")
  expected_output = wc.State(wc_dir,
                             {'A_COPY/D/H/psi' : Item(verb='Sending')})
  wc_status.tweak('A_COPY/D/H/psi', wc_rev=3)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)
  wc_disk.tweak('A_COPY/D/H/psi', contents="New content")

  # r4 - modify and commit A_COPY/D/G/rho
  svntest.main.file_write(os.path.join(wc_dir, "A_COPY", "D", "G", "rho"),
                          "New content")
  expected_output = wc.State(wc_dir,
                             {'A_COPY/D/G/rho' : Item(verb='Sending')})
  wc_status.tweak('A_COPY/D/G/rho', wc_rev=4)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)
  wc_disk.tweak('A_COPY/D/G/rho', contents="New content")

  # r5 - modify (add a prop) and commit A_COPY/B/E/beta
  #svntest.main.file_write(os.path.join(wc_dir, "A_COPY", "B", "E", "beta"),
  #                        "New content")
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      A_COPY_beta_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval',
                                     A_COPY_beta_path)
  expected_output = wc.State(wc_dir,
                             {'A_COPY/B/E/beta' : Item(verb='Sending')})
  wc_status.tweak('A_COPY/B/E/beta', wc_rev=5)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)
  wc_disk.tweak('A_COPY/B/E/beta', props={'prop:name' : 'propval'})
  saved_cwd = os.getcwd()

  # Merge r2:4 into A/D
  # Defined as an internal method since we'll be doing this twice.
  short_D_path = shorten_path_kludge(A_D_path)
  def merge_r24_into_A_D():
    # Search for the comment entitled "The Merge Kluge" elsewhere in
    # this file, to understand why we shorten and chdir() below.
    expected_output = wc.State(short_D_path, {
      'H/psi' : Item(status='U '),
      'G/rho' : Item(status='U '),
      })
    expected_status = wc.State(short_D_path, {
      ''        : Item(status=' M', wc_rev=1),
      'G'       : Item(status='  ', wc_rev=1),
      'G/pi'    : Item(status='  ', wc_rev=1),
      'G/rho'   : Item(status='M ', wc_rev=1),
      'G/tau'   : Item(status='  ', wc_rev=1),
      'H'       : Item(status='  ', wc_rev=1),
      'H/chi'   : Item(status='  ', wc_rev=1),
      'H/psi'   : Item(status='M ', wc_rev=1),
      'H/omega' : Item(status='  ', wc_rev=1),
      'gamma'   : Item(status='  ', wc_rev=1),
      })
    expected_disk = wc.State('', {
      ''        : Item(props={SVN_PROP_MERGE_INFO : '/A_COPY/D:3-4'}),
      'G'       : Item(),
      'G/pi'    : Item("This is the file 'pi'.\n"),
      'G/rho'   : Item("New content"),
      'G/tau'   : Item("This is the file 'tau'.\n"),
      'H'       : Item(),
      'H/chi'   : Item("This is the file 'chi'.\n"),
      'H/psi'   : Item("New content"),
      'H/omega' : Item("This is the file 'omega'.\n"),
      'gamma'   : Item("This is the file 'gamma'.\n")
      })
    expected_skip = wc.State(short_D_path, { })

    os.chdir(svntest.main.work_dir)
    svntest.actions.run_and_verify_merge(short_D_path, '2', '4',
                                         sbox.repo_url + '/A_COPY/D',
                                         expected_output,
                                         expected_disk,
                                         expected_status,
                                         expected_skip,
                                         None, None, None, None,
                                         None, 1)
    os.chdir(saved_cwd)

  merge_r24_into_A_D()

  # Merge r4:2 into A/D/H -- Test Area 1, 2a (see comment at top of test).
  # A/D/H should get mergeinfo for A_COPY/D/H with an empty revision range.
  short_H_path = shorten_path_kludge(A_D_H_path)
  expected_output = wc.State(short_H_path, {
    'psi' : Item(status='G '),
    })
  expected_status = wc.State(short_H_path, {
    ''      : Item(status=' M', wc_rev=1),
    'chi'   : Item(status='  ', wc_rev=1),
    'psi'   : Item(status='  ', wc_rev=1),
    'omega' : Item(status='  ', wc_rev=1),
    })
  expected_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A_COPY/D/H:'}),
    'chi'   : Item("This is the file 'chi'.\n"),
    'psi'   : Item("This is the file 'psi'.\n"),
    'omega' : Item("This is the file 'omega'.\n"),
    })
  expected_skip = wc.State(short_H_path, { })

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_H_path, '4', '2',
                                       sbox.repo_url + '/A_COPY/D/H',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Reverse merge r4:2 from A_COPY into A -- Test Area 1, 2a, 2f.
  # This should effectively revert all local mods.  Pre-eilsion A, A/D, and
  # A/D/H all have empty rev range mergeinfo.  A/D/H elides to A/D, which in
  # turn elides to A.  And since A wisn't overriding any ancestor path's merge
  # info, it too "elides".
  short_A_path = shorten_path_kludge(A_path)

  os.chdir(svntest.main.work_dir)
  # Since this merge returns us to the same state returned by
  # setup_branch() there is no need for run_and_verify_merge().
  # run_and_verify_svn('merge') and ran_and_verify_status() covers
  # everything.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(4,
                                       ['G    ' +
                                        os.path.join(short_A_path, "D", "G",
                                                     "rho") + '\n',
                                        svntest.main.merge_notify_line(4, 3)],
                                       3),
                                     [], 'merge', '-r4:2',
                                     sbox.repo_url + '/A_COPY',
                                     short_A_path)
  os.chdir(saved_cwd)

  # Use wc_status from setup_branch()
  svntest.actions.run_and_verify_status(wc_dir, wc_status)

  # Check that mergeinfo elides if the only difference between a child and
  # parent's mergeinfo are paths that exist only in the child and are mapped
  # to empty revision ranges.
  #
  # Manually set some mergeinfo on A/D.
  svntest.actions.run_and_verify_svn(None,
                                     ["property '" + SVN_PROP_MERGE_INFO +
                                      "' set on '" + A_D_path + "'\n"], [],
                                     'ps', SVN_PROP_MERGE_INFO,
                                     '/A_COPY/B:4', A_D_path)

  # Merge r2:3 into A/D/H
  #
  # A/D/H inherits the r4 mergeinfo set above on A/D and also gets merge
  # info for r3.
  expected_output = wc.State(short_H_path, {
    'psi' : Item(status='U '),
    })
  expected_status.tweak('psi', status='M ')
  expected_disk.tweak('', props={SVN_PROP_MERGE_INFO :
                          '/A_COPY/B/H:4\n/A_COPY/D/H:3\n'})
  expected_disk.tweak('psi', contents="New content")

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_H_path, '2', '3',
                                       sbox.repo_url + '/A_COPY/D/H',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Reverse the previous merge -- Test Area 2c.
  #
  # Effectively this leaves A/D/H with empty revision range mergeinfo from
  # A_COPY/D/H and the inherited mergeinfo from A_COPY/B/H.  Since the only
  # difference between the mergeinfo on A/D/H and A/D is the empty range
  # mergeinfo for A_COPY/D/H, A/D/H's mergeinfo should elide to A/D.
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-3, 'G    ' +
                                       os.path.join(short_H_path, "psi") +
                                       '\n'),
                                     [], 'merge', '-r3:2',
                                     sbox.repo_url + '/A_COPY/D/H',
                                     short_H_path)
  os.chdir(saved_cwd)

  # Use wc_status from setup_branch(), the manually set mergeinfo on A/D
  # and the is the only mod we expect to see.
  wc_status.tweak('A/D', status=' M')
  svntest.actions.run_and_verify_status(wc_dir, wc_status)
  expected = svntest.verify.UnorderedOutput(
    ["Properties on '" + A_D_path + "':\n",
     "  " + SVN_PROP_MERGE_INFO + " : /A_COPY/B:4\n",
     "Properties on '" + A_COPY_path + "':\n",
     "  " + SVN_PROP_MERGE_INFO + " : /A:1\n",
     "Properties on '" + A_COPY_beta_path + "':\n",
     "  prop:name : propval\n"])
  svntest.actions.run_and_verify_svn(None, expected, [], 'pl', '-vR', wc_dir)

  # Revert all local changes
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'revert', '--recursive', wc_dir)
  wc_status.tweak('A/D', status='  ')
  svntest.actions.run_and_verify_status(wc_dir, wc_status)

  # Merge r2:4 into A/D/H again and commit it this time.
  merge_r24_into_A_D()
  wc_status.tweak('A/D', 'A/D/H/psi', 'A/D/G/rho', wc_rev=6)
  wc_disk.tweak('A', props={SVN_PROP_MERGE_INFO : '/A_COPY/D:3-4'})
  expected_output = wc.State(wc_dir, {
    'A/D'       : Item(verb='Sending'),
    'A/D/G/rho' : Item(verb='Sending'),
    'A/D/H/psi' : Item(verb='Sending')})
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        wc_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Set some fictional mergeinfo on A
  svntest.actions.run_and_verify_svn(None,
                                     ["property '" + SVN_PROP_MERGE_INFO +
                                      "' set on '" + A_path + "'\n"],
                                     [], 'ps', SVN_PROP_MERGE_INFO, '/Z:3',
                                     A_path)

  # Merge r4:2 into A/D -- Test Area 2b.
  # This leaves A/D with mergeinfo mapped to empty revision ranges only and
  # it's nearest ancestor A with mergeinfo for a disjoint set of paths.  So
  # the mergeinfo should elide.
  os.chdir(svntest.main.work_dir)
  expected = svntest.verify.UnorderedRegexOutput(
    expected_merge_output(4,
      ["U    " + os.path.join(short_D_path, "G", "rho") + "\n",
       "U    " + os.path.join(short_D_path, "H", "psi") + "\n"],
      3), match_all=False)
  svntest.actions.run_and_verify_svn(None, expected, [], 'merge',
                                     '-r4:2', sbox.repo_url + '/A_COPY/D',
                                     short_D_path)
  os.chdir(saved_cwd)

  svntest.actions.run_and_verify_svn(None, [], [], 'pl', '-vR', A_D_path)

  # Revert the last merge.
  expected = svntest.verify.UnorderedOutput(
    ["Reverted '" + A_D_path + "'\n",
     "Reverted '" + rho_path + "'\n",
     "Reverted '" + psi_path + "'\n",
     ])
  svntest.actions.run_and_verify_svn(None, expected, [], 'revert', '-R',
                                     A_D_path)

  # Set some fictional mergeinfo on A/D/gamma
  propval = "/A_COPY:3\n/Z:"
  propval_file = os.path.abspath(os.path.join(sbox.repo_dir, 'prop-val'))
  svntest.main.file_write(propval_file, propval)
  svntest.actions.run_and_verify_svn(None,
                                     ["property '" + SVN_PROP_MERGE_INFO +
                                      "' set on '" + A_path + "'\n"],
                                     [], 'ps', SVN_PROP_MERGE_INFO,
                                     '-F', propval_file, A_path)

  # Merge -r5:4 from A_COPY/D into A/D -- Test Area 2d.
  # This leaves mergeinfo on A/D that differs from the mergeinfo on A
  # only by a path (Z) in the latter mapped to an empty revision range.
  # So full elision should occur.
  short_D_path = shorten_path_kludge(A_D_path)
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-4,
                                       'U    ' +
                                       os.path.join(short_D_path, "G", "rho") +
                                       '\n'),
                                     [], 'merge', '-c-4',
                                     sbox.repo_url + '/A_COPY/D',
                                     short_D_path)
  os.chdir(saved_cwd)
  svntest.actions.run_and_verify_svn(None,[], [], 'pl', '-vR', A_D_path)

  # Revert local changes.
  expected = svntest.verify.UnorderedOutput(
    ["Reverted '" + A_D_path + "'\n",
     "Reverted '" + rho_path + "'\n",
     "Reverted '" + A_path + "'\n",
     ])
  svntest.actions.run_and_verify_svn(None, expected, [], 'revert', '-R',
                                     wc_dir)

  # Create a second disconnected WC.
  other_wc = sbox.add_wc_path('H')
  svntest.actions.duplicate_dir(A_D_H_path, other_wc)
  expected_output = wc.State(other_wc, {})
  expected_status = wc.State(other_wc, {
    ''      : Item(status='  ', wc_rev=6),
    'chi'   : Item(status='  ', wc_rev=6),
    'psi'   : Item(status='  ', wc_rev=6),
    'omega' : Item(status='  ', wc_rev=6),
    })
  expected_disk = wc.State('', {
    ''      : Item(),
    'chi'   : Item("This is the file 'chi'.\n"),
    'psi'   : Item("New content"),
    'omega' : Item("This is the file 'omega'.\n"),
    })
  svntest.actions.run_and_verify_svn(None, ['At revision 6.\n'], [],
                                     'update', other_wc)

  # Merge r5 from /A_COPY/B/E/beta into the second WC's omega.
  other_omega_path = os.path.join(other_wc, "omega")
  short_other_omega_path = shorten_path_kludge(other_omega_path)
  def merge_r5_into_Other_A_D_H_omega():
    # omega gets the mergeinfo from the merge itself, and the inherited
    # mergeinfo from A/D in the repos.
    os.chdir(svntest.main.work_dir)
    svntest.actions.run_and_verify_svn(None,
                                       expected_merge_output(5,
                                         ' U   ' + short_other_omega_path +
                                         '\n'), [], 'merge', '-c5',
                                       sbox.repo_url + '/A_COPY/B/E/beta',
                                       short_other_omega_path)
    os.chdir(saved_cwd)

    # Check omega's status and props.
    expected_status = wc.State(other_wc, {
      ''      : Item(status='  ', wc_rev=6),
      'chi'   : Item(status='  ', wc_rev=6),
      'psi'   : Item(status='  ', wc_rev=6),
      'omega' : Item(status=' M', wc_rev=6),
      })
    svntest.actions.run_and_verify_status(other_wc, expected_status)

    # Check properties with multiline values in eol sensitive manner.
    svntest.actions.check_prop(SVN_PROP_MERGE_INFO, other_omega_path,
                               ['/A_COPY/B/E/beta:5' + os.linesep,
                                '/A_COPY/D/H/omega:3-4'])
    svntest.actions.check_prop('prop:name', other_omega_path, ['propval'])

  merge_r5_into_Other_A_D_H_omega()

  # Reverse the previous merge -- Test Area 2c, 3.
  # This would leave omega with empty rev range info for path
  # 'A_COPY/B/E/beta' and otherwise elidable (to the repos) mergeinfo for
  # path 'A_COPY/D/H/omega', so all the mergeinfo elides.
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-5,
                                       ' G   ' + short_other_omega_path +
                                       '\n'), [], 'merge', '-c-5',
                                     sbox.repo_url + '/A_COPY/B/E/beta',
                                     short_other_omega_path)
  os.chdir(saved_cwd)

  # Check omega's status (no need to check props since file is back to its
  # pristine state and status checks this).
  expected_status.tweak('omega', status='  ')
  svntest.actions.run_and_verify_status(other_wc, expected_status)

  # Once again, merge r5 from /A_COPY/B/E/beta into the second WC's omega.
  merge_r5_into_Other_A_D_H_omega()

  # Merge r4:3 from A_COPY/D/H/omega into the second WC's omega.
  # Use --record-only to setup the mergeinfo we want since this is a
  # no-op merge.
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None, [], [],
                                     'merge', '-c-4',  '--record-only',
                                     sbox.repo_url + '/A_COPY/D/H/omega',
                                     short_other_omega_path)
  os.chdir(saved_cwd)

  # Check omega's status and props.
  expected_status = wc.State(other_wc, {
    ''      : Item(status='  ', wc_rev=6),
    'chi'   : Item(status='  ', wc_rev=6),
    'psi'   : Item(status='  ', wc_rev=6),
    'omega' : Item(status=' M', wc_rev=6),
    })
  svntest.actions.run_and_verify_status(other_wc, expected_status)

  # Check properties with multiline values in eol sensitive manner.
  svntest.actions.check_prop(SVN_PROP_MERGE_INFO, other_omega_path,
                             ['/A_COPY/B/E/beta:5' + os.linesep,
                              '/A_COPY/D/H/omega:3'])
  svntest.actions.check_prop('prop:name', other_omega_path, ['propval'])

  # Reverse the previous merge of r5 -- Test Area 2e.
  # This would leave omega with empty rev range info for path
  # 'A_COPY/B/E/beta' and unelidable mergeinfo for path 'A_COPY/D/H/omega',
  # so we expect partial elision only of the mergeinfo for 'A_COPY/B/E/beta'
  # since that path doesn't exist in A/D/H/omeaga's nearest ancestor.
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-5,
                                      ' G   ' + short_other_omega_path +
                                      '\n'), [], 'merge', '-c-5',
                                     sbox.repo_url + '/A_COPY/B/E/beta',
                                     short_other_omega_path)
  os.chdir(saved_cwd)

  # Check omega's status and props.
  expected_status.tweak('omega', status=' M')
  svntest.actions.run_and_verify_status(other_wc, expected_status)
  svntest.actions.run_and_verify_svn(None,
                                     ["Properties on '" + other_omega_path +
                                      "':\n",
                                      '  ' + SVN_PROP_MERGE_INFO + ' : ' +
                                      '/A_COPY/D/H/omega:3\n'], [],
                                     'pl', '-vR', other_omega_path)

def detect_copy_src_for_target_with_multiple_ancestors(sbox):
  "detect copy src for target with multiple ancestors"

  # This tests that copy source detection is correct in the case where
  # many ancestors of a target exist in the same commit as a copy of target.

  # Copy A/B as A/copy-of-B
  # Copy A/C as A/copy-of-B/C
  # Commit results in r2.
  # From A/copy-of-B/C do merge -g. This merge should implicitly detect the
  # merge source to be A/C.

  sbox.build()
  wc_dir = sbox.wc_dir
  A_path = os.path.join(wc_dir, 'A')
  A_B_path = os.path.join(A_path, 'B')
  A_C_path = os.path.join(A_path, 'C')
  A_copy_of_B_path = os.path.join(A_path, 'copy-of-B')
  A_copy_of_B_C_path = os.path.join(A_copy_of_B_path, 'C')
  svntest.main.run_svn(None, 'cp', A_B_path, A_copy_of_B_path)
  svntest.main.run_svn(None, 'cp', A_C_path, A_copy_of_B_path)
  expected_output = wc.State(wc_dir, {
    'A/copy-of-B'   : Item(verb='Adding'),
    'A/copy-of-B/C' : Item(verb='Adding')
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/copy-of-B'            : Item(status='  ', wc_rev=2),
    'A/copy-of-B/lambda'     : Item(status='  ', wc_rev=2),
    'A/copy-of-B/E'          : Item(status='  ', wc_rev=2),
    'A/copy-of-B/E/alpha'    : Item(status='  ', wc_rev=2),
    'A/copy-of-B/E/beta'     : Item(status='  ', wc_rev=2),
    'A/copy-of-B/F'          : Item(status='  ', wc_rev=2),
    'A/copy-of-B/C'          : Item(status='  ', wc_rev=2),
    })

  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)
  saved_cwd = os.getcwd()
  os.chdir(A_copy_of_B_C_path)
  # Use --record-only to force the setting of mergeinfo since this is
  # a no-op merge.
  svntest.actions.run_and_verify_svn(None, [],
                                     [], 'merge', '-g', '--record-only')
  os.chdir(saved_cwd)

  expected_status.tweak('A/copy-of-B/C',  status=' M')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  svntest.actions.run_and_verify_svn(None, ["/A/C:2\n"], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     A_copy_of_B_C_path)

def prop_add_to_child_with_mergeinfo(sbox):
  "merge adding prop to child of merge target works"

  # Test for Issue #2781 Prop add to child of merge target corrupts WC if
  # child has mergeinfo.

  sbox.build()
  wc_dir = sbox.wc_dir
  expected_disk, expected_status = setup_branch(sbox)

  # Some paths we'll care about
  beta_path = os.path.join(wc_dir, "A", "B", "E", "beta")
  beta_COPY_path = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  B_COPY_path = os.path.join(wc_dir, "A_COPY", "B")

  # Set a non-mergeinfo prop on a file.
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      beta_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', beta_path)
  expected_disk.tweak('A/B/E/beta', props={'prop:name' : 'propval'})
  expected_status.tweak('A/B/E/beta', wc_rev=7)
  expected_output = wc.State(wc_dir,
                             {'A/B/E/beta' : Item(verb='Sending')})
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Merge r4:5 from A/B/E/beta into A_COPY/B/E/beta.
  short_beta_COPY_path = shorten_path_kludge(beta_COPY_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5,
                                       'U    ' + short_beta_COPY_path +'\n'),
                                     [], 'merge', '-c5',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)
  os.chdir(saved_cwd)

  # Merge r6:7 into A_COPY/B.  In issue #2781 this adds a bogus
  # and incomplete entry in A_COPY/B/.svn/entries for 'beta'.
  short_B_COPY_path = shorten_path_kludge(B_COPY_path)
  expected_output = wc.State(short_B_COPY_path, {
    'E/beta' : Item(status=' U'),
    })
  expected_status = wc.State(short_B_COPY_path, {
    ''        : Item(status=' M', wc_rev=2),
    'E'       : Item(status='  ', wc_rev=2),
    'E/alpha' : Item(status='  ', wc_rev=2),
    'E/beta'  : Item(status='MM', wc_rev=2),
    'lambda'  : Item(status='  ', wc_rev=2),
    'F'       : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,7'}),
    'E'       : Item(),
    'E/alpha' : Item("This is the file 'alpha'.\n"),
    'E/beta'  : Item(contents="New content",
                     props={SVN_PROP_MERGE_INFO : '/A/B/E/beta:1,5,7',
                            'prop:name' : 'propval'}),
    'F'       : Item(),
    'lambda'  : Item("This is the file 'lambda'.\n")
    })
  expected_skip = wc.State(short_B_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_B_COPY_path, '6', '7',
                                       sbox.repo_url + \
                                       '/A/B',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)

def diff_repos_does_not_update_mergeinfo(sbox):
  "don't set mergeinfo when merging from another repo"

  # Test for issue #2788.

  sbox.build()
  wc_dir = sbox.wc_dir
  expected_disk, expected_status = setup_branch(sbox)

  # Create a second repository with the same greek tree
  repo_dir = sbox.repo_dir
  repo_url = sbox.repo_url
  other_repo_dir, other_repo_url = sbox.add_repo_path("other")
  svntest.main.copy_repos(repo_dir, other_repo_dir, 6, 1)

  # Merge r3:4 (using implied peg revisions) from 'other' repos into
  # A_COPY/D/G.  Merge should succeed, but no mergeinfo should be set.
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_G_COPY_path = shorten_path_kludge(os.path.join(wc_dir,
                                                       "A_COPY", "D", "G"))
  saved_cwd = os.getcwd()

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(4,
                                      'U    ' +
                                      os.path.join(short_G_COPY_path,
                                                   "rho") + '\n'),
                                     [], 'merge', '-c4',
                                     other_repo_url + '/A/D/G',
                                     short_G_COPY_path)
  os.chdir(saved_cwd)

  # Merge r4:5 (using explicit peg revisions) from 'other' repos into
  # A_COPY/B/E.  Merge should succeed, but no mergeinfo should be set.
  short_E_COPY_path = shorten_path_kludge(os.path.join(wc_dir,
                                                       "A_COPY", "B", "E"))

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5,
                                      'U    ' +
                                      os.path.join(short_E_COPY_path,
                                                   "beta") +'\n'),
                                     [], 'merge',
                                     other_repo_url + '/A/B/E@4',
                                     other_repo_url + '/A/B/E@5',
                                     short_E_COPY_path)
  os.chdir(saved_cwd)

  expected_status.tweak('A_COPY/D/G/rho', 'A_COPY/B/E/beta', status='M ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

def avoid_reflected_revs(sbox):
  "avoid repeated merges for cyclic merging"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2897. ##

  # Create a WC with a single branch
  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True, 1)

  # Some paths we'll care about
  repo_url = sbox.repo_url
  A_path = os.path.join(wc_dir, 'A')  
  A_COPY_path = os.path.join(wc_dir, 'A_COPY')
  tfile1_path = os.path.join(wc_dir, 'A', 'tfile1')
  tfile2_path = os.path.join(wc_dir, 'A', 'tfile2')
  bfile1_path = os.path.join(A_COPY_path, 'bfile1')
  bfile2_path = os.path.join(A_COPY_path, 'bfile2')

  # Contents to be added to files
  tfile1_content = "This is tfile1\n"
  tfile2_content = "This is tfile2\n"
  bfile1_content = "This is bfile1\n"
  bfile2_content = "This is bfile2\n"

  # We'll consider A as the trunk and A_COPY as the feature branch
  # Create a tfile1 in A
  svntest.main.file_write(tfile1_path, tfile1_content)
  svntest.actions.run_and_verify_svn(None, None, [], 'add', tfile1_path)
  expected_output = wc.State(wc_dir, {'A/tfile1' : Item(verb='Adding')})
  wc_status.add({'A/tfile1'     : Item(status='  ', wc_rev=3)})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  # Create a bfile1 in A_COPY
  svntest.main.file_write(bfile1_path, bfile1_content)
  svntest.actions.run_and_verify_svn(None, None, [], 'add', bfile1_path)
  expected_output = wc.State(wc_dir, {'A_COPY/bfile1' : Item(verb='Adding')})
  wc_status.add({'A_COPY/bfile1'     : Item(status='  ', wc_rev=4)})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  # Create one more file in A
  svntest.main.file_write(tfile2_path, tfile2_content)
  svntest.actions.run_and_verify_svn(None, None, [], 'add', tfile2_path)
  expected_output = wc.State(wc_dir, {'A/tfile2' : Item(verb='Adding')})
  wc_status.add({'A/tfile2'     : Item(status='  ', wc_rev=5)})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  short_A_COPY = shorten_path_kludge(A_COPY_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  # Merge r5 from /A to /A_COPY, creating r6
  expected_output = wc.State(short_A_COPY, {
    'tfile2'   : Item(status='A '),
    })
  expected_status = wc.State(short_A_COPY, {
    ''         : Item(status=' M', wc_rev=2),
    'tfile2'   : Item(status='A ', wc_rev='-', copied='+'),
    'bfile1'   : Item(status='  ', wc_rev=4),
    'mu'       : Item(status='  ', wc_rev=2),
    'C'        : Item(status='  ', wc_rev=2),
    'D'        : Item(status='  ', wc_rev=2),
    'B'        : Item(status='  ', wc_rev=2),
    'B/lambda' : Item(status='  ', wc_rev=2),
    'B/E'      : Item(status='  ', wc_rev=2),
    'B/E/alpha': Item(status='  ', wc_rev=2),
    'B/E/beta' : Item(status='  ', wc_rev=2),
    'B/F'      : Item(status='  ', wc_rev=2),
    'D/gamma'  : Item(status='  ', wc_rev=2),
    'D/G'      : Item(status='  ', wc_rev=2),
    'D/G/pi'   : Item(status='  ', wc_rev=2),
    'D/G/rho'  : Item(status='  ', wc_rev=2),
    'D/G/tau'  : Item(status='  ', wc_rev=2),
    'D/H'      : Item(status='  ', wc_rev=2),
    'D/H/chi'  : Item(status='  ', wc_rev=2),
    'D/H/omega': Item(status='  ', wc_rev=2),
    'D/H/psi'  : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A:1,5'}),
    'tfile2'   : Item(tfile2_content),
    'bfile1'   : Item(bfile1_content),
    'mu'       : Item("This is the file 'mu'.\n"),
    'C'        : Item(),
    'D'        : Item(),
    'B'        : Item(),
    'B/lambda' : Item("This is the file 'lambda'.\n"),
    'B/E'      : Item(),
    'B/E/alpha': Item("This is the file 'alpha'.\n"),
    'B/E/beta' : Item("This is the file 'beta'.\n"),
    'B/F'      : Item(),
    'D/gamma'  : Item("This is the file 'gamma'.\n"),
    'D/G'      : Item(),
    'D/G/pi'   : Item("This is the file 'pi'.\n"),
    'D/G/rho'  : Item("This is the file 'rho'.\n"),
    'D/G/tau'  : Item("This is the file 'tau'.\n"),
    'D/H'      : Item(),
    'D/H/chi'  : Item("This is the file 'chi'.\n"),
    'D/H/omega': Item("This is the file 'omega'.\n"),
    'D/H/psi'  : Item("This is the file 'psi'.\n"),
    })
  expected_skip = wc.State(short_A_COPY, {})

  svntest.actions.run_and_verify_merge(short_A_COPY, '4', '5',
                                       sbox.repo_url + '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Sync up with the trunk ie., A
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)
  expected_output = wc.State(wc_dir, {
    'A_COPY'        : Item(verb='Sending'),
    'A_COPY/tfile2' : Item(verb='Adding'),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        None, None, None, None,
                                        None, None, wc_dir)
  os.chdir(svntest.main.work_dir)

  # Merge r3 from /A to /A_COPY, creating r7
  expected_output = wc.State(short_A_COPY, {
    'tfile1'   : Item(status='A '),
    })
  expected_status.tweak(wc_rev=5)
  expected_status.tweak('', wc_rev=6)
  expected_status.tweak('tfile2', status='  ', copied=None, wc_rev=6)
  expected_status.add({
   'tfile1'    : Item(status='A ', wc_rev='-', copied='+'),
    })
  expected_disk.tweak('', props={SVN_PROP_MERGE_INFO : '/A:1,3,5'})
  expected_disk.add({
    'tfile1'   : Item(tfile1_content),
    })

  svntest.actions.run_and_verify_merge(short_A_COPY, '2', '3',
                                       sbox.repo_url + '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)

  os.chdir(saved_cwd)

  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)
  expected_output = wc.State(wc_dir, {
    'A_COPY'        : Item(verb='Sending'),
    'A_COPY/tfile1' : Item(verb='Adding'),    
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        None, None, None, None,
                                        None, None, wc_dir)

  # Add bfile2 to A_COPY
  svntest.main.file_write(bfile2_path, bfile2_content)
  svntest.actions.run_and_verify_svn(None, None, [], 'add', bfile2_path)
  expected_output = wc.State(wc_dir, {'A_COPY/bfile2' : Item(verb='Adding')})
  wc_status.tweak(wc_rev=6)
  wc_status.add({
    'A_COPY/bfile2' : Item(status='  ', wc_rev=8),
    'A_COPY'        : Item(status='  ', wc_rev=7),
    'A_COPY/tfile2' : Item(status='  ', wc_rev=6),
    'A_COPY/tfile1' : Item(status='  ', wc_rev=7),    
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  # Merge 2:8 from A_COPY(feature branch) to A(trunk).
  short_A_path = shorten_path_kludge(A_path)
  expected_output = wc.State(short_A_path, {
    'bfile2' : Item(status='A '),
    'bfile1' : Item(status='A '),
    })
  expected_status = wc.State(short_A_path, {
    ''          : Item(status=' M', wc_rev=6),
    'bfile2'    : Item(status='A ', wc_rev='-', copied='+'),
    'bfile1'    : Item(status='A ', wc_rev='-', copied='+'),
    'tfile2'    : Item(status='  ', wc_rev=6),
    'tfile1'    : Item(status='  ', wc_rev=6),
    'mu'        : Item(status='  ', wc_rev=6),
    'C'         : Item(status='  ', wc_rev=6),
    'D'         : Item(status='  ', wc_rev=6),
    'B'         : Item(status='  ', wc_rev=6),
    'B/lambda'  : Item(status='  ', wc_rev=6),
    'B/E'       : Item(status='  ', wc_rev=6),
    'B/E/alpha' : Item(status='  ', wc_rev=6),
    'B/E/beta'  : Item(status='  ', wc_rev=6),
    'B/F'       : Item(status='  ', wc_rev=6),
    'D/gamma'   : Item(status='  ', wc_rev=6),
    'D/G'       : Item(status='  ', wc_rev=6),
    'D/G/pi'    : Item(status='  ', wc_rev=6),
    'D/G/rho'   : Item(status='  ', wc_rev=6),
    'D/G/tau'   : Item(status='  ', wc_rev=6),
    'D/H'       : Item(status='  ', wc_rev=6),
    'D/H/chi'   : Item(status='  ', wc_rev=6),
    'D/H/omega' : Item(status='  ', wc_rev=6),
    'D/H/psi'   : Item(status='  ', wc_rev=6),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A_COPY:3-5,8'}),
    'bfile2'    : Item(bfile2_content),
    'bfile1'    : Item(bfile1_content),
    'tfile2'    : Item(tfile2_content),
    'tfile1'    : Item(tfile1_content),
    'mu'        : Item("This is the file 'mu'.\n"),
    'C'         : Item(),
    'D'         : Item(),
    'B'         : Item(),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/E'       : Item(),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("This is the file 'beta'.\n"),
    'B/F'       : Item(),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D/G'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("This is the file 'rho'.\n"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    'D/H'       : Item(),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
    'D/H/omega' : Item("This is the file 'omega'.\n"),
    'D/H/psi'   : Item("This is the file 'psi'.\n"),
    })

  expected_skip = wc.State(short_A_path, {})
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  svntest.actions.run_and_verify_merge(short_A_path, '2', '8',
                                       sbox.repo_url + '/A_COPY',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

# Test for issue #2893
# Handle merge info for portions of a tree not checked out due to
# insufficient authz.
def mergeinfo_and_skipped_paths(sbox):
  "skipped paths get overriding mergeinfo"

  # Test that we override the mergeinfo for child paths which weren't
  # actually merged because they were skipped.
  #
  # This test covers paths skipped because:
  #
  #   1) The source of a merge is inaccessible due to authz restrictions.
  #   2) Destination of merge is inaccessible due to authz restrictions.
  #   3) Source *and* destination of merge is inaccessible due to authz
  #      restrictions.
  #   4) File path is versioned but is missing from disk due to OS deletion.
  #      This isn't technically part of issue #2893 but we handle this case
  #      and it didn't warrant its own test).
  #
  # Eventually we should also test(?):
  #
  #   5) Dir path is versioned but is missing from disk due to an OS deletion.

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, False, 3)

  # Create a restrictive authz where part of the merge source and part
  # of the target are inaccesible.
  write_restrictive_svnserve_conf(sbox.repo_dir)
  write_authz_file(sbox, {"/"               : svntest.main.wc_author +"=rw",
                          # Make a directory in the merge source inaccessible.
                          "/A/B/E"            : svntest.main.wc_author + "=",
                          # Make a file and dir in the merge destination
                          # inaccessible.
                          "/A_COPY_2/D/H/psi" : svntest.main.wc_author + "=",
                          "/A_COPY_2/D/G" : svntest.main.wc_author + "=",
                          # Make the source and destination inaccessible.
                          "/A_COPY_3/B/E"     : svntest.main.wc_author + "=",
                          })

  # Checkout just the branch under the newly restricted authz.
  wc_restricted = sbox.add_wc_path('restricted')
  svntest.actions.run_and_verify_svn(None, None, [], 'checkout',
                                     sbox.repo_url,
                                     wc_restricted)

  # Some paths we'll use in the second WC.
  A_COPY_path = os.path.join(wc_restricted, "A_COPY")
  A_COPY_2_path = os.path.join(wc_restricted, "A_COPY_2")
  A_COPY_3_path = os.path.join(wc_restricted, "A_COPY_3")
  omega_path = os.path.join(wc_restricted, "A_COPY", "D", "H", "omega")

  # Restrict access to some more of the merge destination the
  # old fashioned way, delete it via the OS.
  ### TODO: Delete a versioned directory?
  os.remove(omega_path)

  # Merge r4:8 into the restricted WC's A_COPY.
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  #
  # We expect A_COPY/B/E to be skipped because we can't access the source
  # and A_COPY/D/H/omega because it is missing.  Since we have A_COPY/B/E
  # we should override it's inherited mergeinfo, giving it just what it
  # inherited from A_COPY before the merge.  omega is missing, but since
  # it is a file we can record the fact that it is missing in its parent
  # directory A_COPY/D/H.
  short_path = shorten_path_kludge(A_COPY_path)
  expected_output = wc.State(short_path, {
    'D/G/rho'   : Item(status='U '),
    'D/H/psi'   : Item(status='U '),
    })
  expected_status = wc.State(short_path, {
    ''          : Item(status=' M', wc_rev=8),
    'D/H/chi'   : Item(status='  ', wc_rev=8),
    'D/H/psi'   : Item(status='M ', wc_rev=8),
    'D/H/omega' : Item(status='!M', wc_rev=8),
    'D/H'       : Item(status='  ', wc_rev=8),
    'D/G/pi'    : Item(status='  ', wc_rev=8),
    'D/G/rho'   : Item(status='M ', wc_rev=8),
    'D/G/tau'   : Item(status='  ', wc_rev=8),
    'D/G'       : Item(status='  ', wc_rev=8),
    'D/gamma'   : Item(status='  ', wc_rev=8),
    'D'         : Item(status='  ', wc_rev=8),
    'B/lambda'  : Item(status='  ', wc_rev=8),
    'B/E'       : Item(status=' M', wc_rev=8),
    'B/E/alpha' : Item(status='  ', wc_rev=8),
    'B/E/beta'  : Item(status='  ', wc_rev=8),
    'B/F'       : Item(status='  ', wc_rev=8),
    'B'         : Item(status='  ', wc_rev=8),
    'mu'        : Item(status='  ', wc_rev=8),
    'C'         : Item(status='  ', wc_rev=8),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,5-8'}),
    'D/H/psi'   : Item("New content"),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
     # 'D/H/omega' : run_and_verify_merge() doesn't support checking
     #               the props on a missing path, so we do that
     #               manually (see below).
    'D/H'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("New content"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    'D/G'       : Item(),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D'         : Item(),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/E'       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:1'}),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("This is the file 'beta'.\n"),
    'B/F'       : Item(),
    'B'         : Item(),
    'mu'        : Item("This is the file 'mu'.\n"),
    'C'         : Item(),
    })
  expected_skip = wc.State(short_path, {
    'D/H/omega' : Item(),
    'B/E'       : Item(),
    })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_path, '4', '8',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Manually check the props on A_COPY/D/H/omega.
  svntest.actions.run_and_verify_svn(None,
                                     ["Properties on '" + omega_path + "':\n",
                                      '  ' + SVN_PROP_MERGE_INFO + ' : ' +
                                      '/A/D/H/omega:1\n'],
                                     [], 'pl', '-vR', omega_path)

  # Merge r4:8 into the restricted WC's A_COPY_2.
  #
  # As before we expect A_COPY_2/B/E to be skipped because we can't access the
  # source but now the destination paths A_COPY_2/D/G, A_COPY_2/D/G/rho, and
  # A_COPY_2/D/H/psi should also be skipped because our test user doesn't have
  # access.
  #
  # After the merge the parents of the missing dest paths, A_COPY_2/D and
  # A_COPY_2/D/H get non-inheritable mergeinfo.  Those parents children that
  # *are* present, A_COPY_2/D/gamma, A_COPY_2/D/H/chi, and A_COPY_2/D/H/omega
  # get their own mergeinfo.  Note that A_COPY_2/D/H is both the parent of
  # a missing child and the sibling of missing child, but the former always
  # takes precedence in terms of getting *non*-inheritable mergeinfo.
  short_path = shorten_path_kludge(A_COPY_2_path)
  expected_output = wc.State(short_path, {
    'D/H/omega' : Item(status='U '),
    })
  expected_status = wc.State(short_path, {
    ''          : Item(status=' M', wc_rev=8),
    'D/H/chi'   : Item(status=' M', wc_rev=8),
    'D/H/omega' : Item(status='MM', wc_rev=8),
    'D/H'       : Item(status=' M', wc_rev=8),
    'D/gamma'   : Item(status=' M', wc_rev=8),
    'D'         : Item(status=' M', wc_rev=8),
    'B/lambda'  : Item(status='  ', wc_rev=8),
    'B/E'       : Item(status=' M', wc_rev=8),
    'B/E/alpha' : Item(status='  ', wc_rev=8),
    'B/E/beta'  : Item(status='  ', wc_rev=8),
    'B/F'       : Item(status='  ', wc_rev=8),
    'B'         : Item(status='  ', wc_rev=8),
    'mu'        : Item(status='  ', wc_rev=8),
    'C'         : Item(status='  ', wc_rev=8),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1-2,5-8'}),
    'D/H/omega' : Item("New content",
                       props={SVN_PROP_MERGE_INFO : '/A/D/H/omega:1-2,5-8'}),
    'D/H/chi'   : Item("This is the file 'chi'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/D/H/chi:1-2,5-8'}),
    'D/H'       : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1-2,5-8*'}),
    'D/gamma'   : Item("This is the file 'gamma'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/D/gamma:1-2,5-8'}),
    'D'         : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1-2,5-8*'}),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/E'       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:1-2'}),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("This is the file 'beta'.\n"),
    'B/F'       : Item(),
    'B'         : Item(),
    'mu'        : Item("This is the file 'mu'.\n"),
    'C'         : Item(),
    })
  expected_skip = wc.State(short_path, {
    'D/G'     : Item(),
    'D/G/rho' : Item(),
    'D/H/psi' : Item(),
    'B/E'     : Item(),
    })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_path, '4', '8',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1, 0)
  os.chdir(saved_cwd)

  # Merge r5:7 into the restricted WC's A_COPY_3.
  #
  # Again A_COPY_3/B/E should be skipped, but because we can't access the
  # source *or* the destination we expect its parent A_COPY_3/B to get
  # non-inheritable mergeinfo and its two existing siblings, A_COPY_3/B/F
  # and A_COPY_3/B/lambda to get their own mergeinfo.
  short_path = shorten_path_kludge(A_COPY_3_path)
  expected_output = wc.State(short_path, {
    'D/G/rho' : Item(status='U '),
    })
  expected_status = wc.State(short_path, {
    ''          : Item(status=' M', wc_rev=8),
    'D/H/chi'   : Item(status='  ', wc_rev=8),
    'D/H/omega' : Item(status='  ', wc_rev=8),
    'D/H/psi'   : Item(status='  ', wc_rev=8),
    'D/H'       : Item(status='  ', wc_rev=8),
    'D/gamma'   : Item(status='  ', wc_rev=8),
    'D'         : Item(status='  ', wc_rev=8),
    'D/G'       : Item(status='  ', wc_rev=8),
    'D/G/pi'    : Item(status='  ', wc_rev=8),
    'D/G/rho'   : Item(status='M ', wc_rev=8),
    'D/G/tau'   : Item(status='  ', wc_rev=8),
    'B/lambda'  : Item(status=' M', wc_rev=8),
    'B/F'       : Item(status=' M', wc_rev=8),
    'B'         : Item(status=' M', wc_rev=8),
    'mu'        : Item(status='  ', wc_rev=8),
    'C'         : Item(status='  ', wc_rev=8),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1-3,6-7'}),
    'D/H/omega' : Item("This is the file 'omega'.\n"),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
    'D/H/psi'   : Item("This is the file 'psi'.\n"),
    'D/H'       : Item(),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D'         : Item(),
    'D/G'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("New content"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    'B/lambda'  : Item("This is the file 'lambda'.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/B/lambda:1-3,6-7'}),
    'B/F'       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:1-3,6-7'}),
    'B'         : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-3,6-7*'}),
    'mu'        : Item("This is the file 'mu'.\n"),
    'C'         : Item(),
    })
  expected_skip = wc.State(short_path, {'B/E' : Item()})
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_path, '5', '7',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1, 0)
  os.chdir(saved_cwd)

def update_loses_mergeinfo(sbox):
  "update does not merge mergeinfo"

  """
  When a working copy receives a fresh svn:mergeinfo property due to update,
  and working copy has some local mergeinfo(yet to be added), local mergeinfo
  is left unchanged.
  """

  sbox.build()
  wc_dir = sbox.wc_dir
  A_C_wc_dir = wc_dir + '/A/C'
  A_B_url = sbox.repo_url + '/A/B'
  A_B_J_url = sbox.repo_url + '/A/B/J'
  A_B_K_url = sbox.repo_url + '/A/B/K'
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 2.\n'],
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'mkdir', '-m', 'rev 2', A_B_J_url)
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 3.\n'],
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'mkdir', '-m', 'rev 3', A_B_K_url)

  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  short_A_C_wc_dir = shorten_path_kludge(A_C_wc_dir)
  expected_output = wc.State(short_A_C_wc_dir, {'J' : Item(status='A ')})
  expected_disk = wc.State('', {
    'J'       : Item(),
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:2'}),
    })
  expected_status = wc.State(short_A_C_wc_dir,
                             { ''    : Item(wc_rev=1, status=' M'),
                               'J'   : Item(status='A ',
                                            wc_rev='-', copied='+')
                             }
                            )
  expected_skip = wc.State('', { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_C_wc_dir, '1', '2',
                                       A_B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       check_props=1)
  os.chdir(saved_cwd)
  expected_output = wc.State(A_C_wc_dir, {
    ''  : Item(verb='Sending'),
    'J' : Item(verb='Adding')
    })
  expected_status = wc.State(A_C_wc_dir,
                             { ''    : Item(status='  ', wc_rev=4),
                               'J'   : Item(status='  ', wc_rev=4)
                             }
                            )
  svntest.actions.run_and_verify_commit(A_C_wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        A_C_wc_dir)

  other_A_C_wc_dir = other_wc + '/A/C'
  short_other_A_C_wc_dir = shorten_path_kludge(other_A_C_wc_dir)
  expected_output = wc.State(short_other_A_C_wc_dir, {'K' : Item(status='A ')})
  expected_disk = wc.State('', {
    'K'       : Item(),
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:3'}),
    })
  expected_status = wc.State(short_other_A_C_wc_dir,
                             { ''    : Item(wc_rev=1, status=' M'),
                               'K'   : Item(status='A ',
                                            wc_rev='-', copied='+')
                             }
                            )
  expected_skip = wc.State('', { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_other_A_C_wc_dir, '2', '3',
                                       A_B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       check_props=1)
  os.chdir(saved_cwd)
  expected_output = wc.State(other_A_C_wc_dir,
                             {'J' : Item(status='A '),
                              ''  : Item(status=' G')
                             }
                            )
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:2-3'}),
    'J'       : Item(),
    'K'       : Item(),
    })
  expected_status = wc.State(other_A_C_wc_dir,
                             { ''    : Item(wc_rev=4, status=' M'),
                               'J'   : Item(status='  ', wc_rev='4'),
                               'K'   : Item(status='A ',
                                            wc_rev='-', copied='+')
                             }
                            )
  svntest.actions.run_and_verify_update(other_A_C_wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        check_props=1)


def merge_loses_mergeinfo(sbox):
  "merge does not merge mergeinfo"

  """
  When a working copy has no mergeinfo(due to local full revert of all merges),
  and merge is attempted for someother revision rX, The new mergeinfo should be
  /merge/src: rX not all the reverted ones reappearing along with rX.
  """

  sbox.build()
  wc_dir = sbox.wc_dir
  A_C_wc_dir = wc_dir + '/A/C'
  A_B_url = sbox.repo_url + '/A/B'
  A_B_J_url = sbox.repo_url + '/A/B/J'
  A_B_K_url = sbox.repo_url + '/A/B/K'
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 2.\n'],
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'mkdir', '-m', 'rev 2', A_B_J_url)
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 3.\n'],
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'mkdir', '-m', 'rev 3', A_B_K_url)

  short_A_C_wc_dir = shorten_path_kludge(A_C_wc_dir)
  expected_output = wc.State(short_A_C_wc_dir, {'J' : Item(status='A ')})
  expected_disk = wc.State('', {
    'J'       : Item(),
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:2'}),
    })
  expected_status = wc.State(short_A_C_wc_dir,
                             { ''    : Item(wc_rev=1, status=' M'),
                               'J'   : Item(status='A ',
                                            wc_rev='-', copied='+')
                             }
                            )
  expected_skip = wc.State('', { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_C_wc_dir, '1', '2',
                                       A_B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       check_props=1)
  os.chdir(saved_cwd)
  expected_output = wc.State(A_C_wc_dir, {
    ''  : Item(verb='Sending'),
    'J' : Item(verb='Adding')
    })
  expected_status = wc.State(A_C_wc_dir,
                             { ''    : Item(status='  ', wc_rev=4),
                               'J'   : Item(status='  ', wc_rev=4)
                             }
                            )
  svntest.actions.run_and_verify_commit(A_C_wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        A_C_wc_dir)
  expected_output = wc.State(short_A_C_wc_dir, {'J' : Item(status='D ')})
  expected_disk = wc.State('', {'J': Item()})
  expected_status = wc.State(short_A_C_wc_dir,
                             { ''    : Item(wc_rev=4, status=' M'),
                               'J'   : Item(wc_rev=4, status='D ')
                             }
                            )
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_C_wc_dir, '2', '1',
                                       A_B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       check_props=1)
  os.chdir(saved_cwd)

  expected_output = wc.State(short_A_C_wc_dir, {'K' : Item(status='A ')})
  expected_disk = wc.State('', {
    'K'       : Item(),
    'J'       : Item(),
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:3'}),
    })
  expected_status = wc.State(short_A_C_wc_dir,
                             { ''    : Item(wc_rev=4, status=' M'),
                               'K'   : Item(status='A ',
                                            wc_rev='-', copied='+'),
                               'J'   : Item(wc_rev=4, status='D ')
                             }
                            )
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_C_wc_dir, '2', '3',
                                       A_B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       check_props=1)

def single_file_replace_style_merge_capability(sbox):
  "replace-style merge capability for a single file"

  # Test for issue #2853, do_single_file_merge() lacks "Replace-style
  # merge" capability

  sbox.build()
  wc_dir = sbox.wc_dir
  iota_path = os.path.join(wc_dir, 'iota')
  mu_path = os.path.join(wc_dir, 'A', 'mu')

  # delete mu and replace it with a copy of iota
  svntest.main.run_svn(None, 'rm', mu_path)
  svntest.main.run_svn(None, 'mv', iota_path, mu_path)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', status='  ', wc_rev=2)
  expected_status.remove('iota')
  expected_output = svntest.wc.State(wc_dir, {
    'iota': Item(verb='Deleting'),
    'A/mu': Item(verb='Replacing'),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Merge the file mu alone to rev1
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-2,
                                       ['D    ' + mu_path + '\n',
                                        'A    ' + mu_path + '\n']),
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge',
                                     mu_path + '@2',
                                     mu_path + '@1',
                                     mu_path)

# Test for issue 2786 fix.
def merge_to_out_of_date_target(sbox):
  "merge to ood path can lead to inaccurate mergeinfo"

  # Create a WC with a branch.
  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, False, 1)

  # Make second working copy
  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  # Some paths we'll care about
  A_COPY_H_path = os.path.join(wc_dir, "A_COPY", "D", "H")
  other_A_COPY_H_path = os.path.join(other_wc, "A_COPY", "D", "H")

  # Merge -c3 into A_COPY/D/H of first WC.
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_H_COPY_path = shorten_path_kludge(A_COPY_H_path)
  expected_output = wc.State(short_H_COPY_path, {
    'psi' : Item(status='U ')
    })
  expected_status = wc.State(short_H_COPY_path, {
    ''      : Item(status=' M', wc_rev=2),
    'psi'   : Item(status='M ', wc_rev=2),
    'omega' : Item(status='  ', wc_rev=2),
    'chi'   : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,3'}),
    'psi'   : Item("New content"),
    'omega' : Item("This is the file 'omega'.\n"),
    'chi'   : Item("This is the file 'chi'.\n"),
    })
  expected_skip = wc.State(short_H_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_H_COPY_path, '2', '3',
                                       sbox.repo_url + '/A/D/H',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Commit merge to first WC.
  wc_status.tweak('A_COPY/D/H/psi', 'A_COPY/D/H', wc_rev=7)
  expected_output = svntest.wc.State(wc_dir, {
    'A_COPY/D/H'    : Item(verb='Sending'),
    'A_COPY/D/H/psi': Item(verb='Sending'),
    })
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        wc_status,
                                        None, None, None, None, None,
                                        wc_dir)

  # Merge -c6 into A_COPY/D/H of other WC.
  short_H_COPY_path = shorten_path_kludge(other_A_COPY_H_path)
  expected_output = wc.State(short_H_COPY_path, {
    'omega' : Item(status='U ')
    })
  expected_status = wc.State(short_H_COPY_path, {
    ''      : Item(status=' M', wc_rev=2),
    'psi'   : Item(status='  ', wc_rev=2),
    'omega' : Item(status='M ', wc_rev=2),
    'chi'   : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,6'}),
    'psi'   : Item("This is the file 'psi'.\n"),
    'omega' : Item("New content"),
    'chi'   : Item("This is the file 'chi'.\n"),
    })
  expected_skip = wc.State(short_H_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_H_COPY_path, '5', '6',
                                       sbox.repo_url + '/A/D/H',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

  # Update A_COPY/D/H in other WC.  Local mergeinfo for r6 on A_COPY/D/H
  # should be *merged* with r3 from first WC.
  expected_output = svntest.wc.State(other_A_COPY_H_path, {
    ''     : Item(status=' G'),
    'psi' : Item(status='U ')
    })
  other_disk = wc.State('', {
    ''      : Item(props={SVN_PROP_MERGE_INFO : '/A/D/H:1,3,6'}),
    'psi'   : Item(contents="New content"),
    'chi'   : Item("This is the file 'chi'.\n"),
    'omega' : Item(contents="New content"),
    })
  other_status = wc.State(other_A_COPY_H_path,{
    ''      : Item(wc_rev=7, status=' M'),
    'chi'   : Item(wc_rev=7, status='  '),
    'psi'   : Item(wc_rev=7, status='  '),
    'omega' : Item(wc_rev=7, status='M ')
    })
  svntest.actions.run_and_verify_update(other_A_COPY_H_path,
                                        expected_output,
                                        other_disk,
                                        other_status,
                                        check_props=1)

def merge_with_depth_files(sbox):
  "merge test for --depth files"

  sbox.build()
  wc_dir = sbox.wc_dir
  repo_url = sbox.repo_url

  # Some paths we'll care about
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  gamma_path = os.path.join(wc_dir, 'A', 'D', 'gamma')
  Acopy_path = os.path.join(wc_dir, 'A_copy')
  Acopy_mu_path = os.path.join(wc_dir, 'A_copy', 'mu')
  A_url = repo_url + '/A'
  Acopy_url = repo_url + '/A_copy'

  # Copy A_url to A_copy_url
  svntest.actions.run_and_verify_svn(None, None, [], 'cp',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url, Acopy_url,
                                     '-m', 'create a new copy of A')

  svntest.main.file_write(mu_path, "this is file 'mu' modified.\n")
  svntest.main.file_write(gamma_path, "this is file 'gamma' modified.\n")

  # Create expected output tree for commit
  expected_output = wc.State(wc_dir, {
    'A/mu'        : Item(verb='Sending'),
    'A/D/gamma'   : Item(verb='Sending'),
    })

  # Create expected status tree for commit
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/mu'          : Item(status='  ', wc_rev=3),
    'A/D/gamma'     : Item(status='  ', wc_rev=3),
    })

  # Commit the modified contents
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir)

  # Update working copy
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'up', Acopy_path)

  # Merge r1:3 into A_copy with --depth files.  The merge only affects
  # 'A_copy' and its one file child 'mu', so 'A_copy' gets non-inheritable
  # mergeinfo for -r1:3 and 'mu' gets its own complete set of mergeinfo:
  # r1 from its parent, and r1:3 from the merge itself.
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_A_COPY_path = shorten_path_kludge(Acopy_path)
  expected_output = wc.State(short_A_COPY_path, {
    'mu'   : Item(status='U '),
    })
  expected_status = wc.State(short_A_COPY_path, {
    ''          : Item(status=' M'),
    'B'         : Item(status='  '),
    'mu'        : Item(status='MM'),
    'B/E'       : Item(status='  '),
    'B/E/alpha' : Item(status='  '),
    'B/E/beta'  : Item(status='  '),
    'B/lambda'  : Item(status='  '),
    'B/F'       : Item(status='  '),
    'C'         : Item(status='  '),
    'D'         : Item(status='  '),
    'D/G'       : Item(status='  '),
    'D/G/pi'    : Item(status='  '),
    'D/G/rho'   : Item(status='  '),
    'D/G/tau'   : Item(status='  '),
    'D/gamma'   : Item(status='  '),
    'D/H'       : Item(status='  '),
    'D/H/chi'   : Item(status='  '),
    'D/H/psi'   : Item(status='  '),
    'D/H/omega' : Item(status='  '),
    })
  expected_status.tweak(wc_rev=3)
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,2-3*'}),
    'B'         : Item(),
    'mu'        : Item("this is file 'mu' modified.\n",
                       props={SVN_PROP_MERGE_INFO : '/A/mu:1-3'}),
    'B/E'       : Item(),
    'B/E/alpha' : Item("This is the file 'alpha'.\n"),
    'B/E/beta'  : Item("This is the file 'beta'.\n"),
    'B/lambda'  : Item("This is the file 'lambda'.\n"),
    'B/F'       : Item(),
    'C'         : Item(),
    'D'         : Item(),
    'D/G'       : Item(),
    'D/G/pi'    : Item("This is the file 'pi'.\n"),
    'D/G/rho'   : Item("This is the file 'rho'.\n"),
    'D/G/tau'   : Item("This is the file 'tau'.\n"),
    'D/gamma'   : Item("This is the file 'gamma'.\n"),
    'D/H'       : Item(),
    'D/H/chi'   : Item("This is the file 'chi'.\n"),
    'D/H/psi'   : Item("This is the file 'psi'.\n"),
    'D/H/omega' : Item("This is the file 'omega'.\n"),
    })
  expected_skip = wc.State(short_A_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY_path, '1', '3',
                                       sbox.repo_url + '/A',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1, 1,
                                       '--depth', 'files')
  os.chdir(saved_cwd)

def merge_fails_if_subtree_is_deleted_on_src(sbox):
  "merge fails if subtree is deleted on src"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2876. ##

  # Create a WC and for some paths and URLs
  sbox.build()
  wc_dir = sbox.wc_dir
  repo_url = sbox.repo_url
  Acopy_path = os.path.join(wc_dir, 'A_copy')
  gamma_path = os.path.join(wc_dir, 'A', 'D', 'gamma')
  Acopy_gamma_path = os.path.join(wc_dir, 'A_copy', 'D', 'gamma')
  A_url = repo_url + '/A'
  Acopy_url = repo_url + '/A_copy'

  # Contents to be added to 'gamma'
  new_content = "line1\nline2\nline3\nline4\nline5\n"

  svntest.main.file_write(gamma_path, new_content)

  # Create expected output tree for commit
  expected_output = wc.State(wc_dir, {
    'A/D/gamma' : Item(verb='Sending'),
    })

  # Create expected status tree for commit
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/D/gamma', wc_rev=2)

  # Commit the new content
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  svntest.actions.run_and_verify_svn(None, None, [], 'cp', A_url, Acopy_url,
                                     '-m', 'create a new copy of A')

  # Update working copy
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  svntest.main.file_substitute(gamma_path, "line1", "this is line1")
  # Create expected output tree for commit
  expected_output = wc.State(wc_dir, {
    'A/D/gamma' : Item(verb='Sending'),
    })

  # Create expected status tree for commit
  expected_status.tweak(wc_rev=3)
  expected_status.tweak('A/D/gamma', wc_rev=4)
  expected_status.add({
    'A_copy'          : Item(status='  ', wc_rev=3),
    'A_copy/B'        : Item(status='  ', wc_rev=3),
    'A_copy/B/lambda' : Item(status='  ', wc_rev=3),
    'A_copy/B/E'      : Item(status='  ', wc_rev=3),
    'A_copy/B/E/alpha': Item(status='  ', wc_rev=3),
    'A_copy/B/E/beta' : Item(status='  ', wc_rev=3),
    'A_copy/B/F'      : Item(status='  ', wc_rev=3),
    'A_copy/mu'       : Item(status='  ', wc_rev=3),
    'A_copy/C'        : Item(status='  ', wc_rev=3),
    'A_copy/D'        : Item(status='  ', wc_rev=3),
    'A_copy/D/gamma'  : Item(status='  ', wc_rev=3),
    'A_copy/D/G'      : Item(status='  ', wc_rev=3),
    'A_copy/D/G/pi'   : Item(status='  ', wc_rev=3),
    'A_copy/D/G/rho'  : Item(status='  ', wc_rev=3),
    'A_copy/D/G/tau'  : Item(status='  ', wc_rev=3),
    'A_copy/D/H'      : Item(status='  ', wc_rev=3),
    'A_copy/D/H/chi'  : Item(status='  ', wc_rev=3),
    'A_copy/D/H/omega': Item(status='  ', wc_rev=3),
    'A_copy/D/H/psi'  : Item(status='  ', wc_rev=3),
    })

  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  # Delete A/D/gamma from working copy
  svntest.actions.run_and_verify_svn(None, None, [], 'delete', gamma_path)
  # Create expected output tree for commit
  expected_output = wc.State(wc_dir, {
    'A/D/gamma' : Item(verb='Deleting'),
    })

  expected_status.remove('A/D/gamma')

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None, None, None,
                                        wc_dir, wc_dir)

  svntest.actions.run_and_verify_svn(None, expected_merge_output(3,
                                     'U    ' + Acopy_gamma_path + '\n', 4),
                                     [], 'merge', '-r1:4',
                                     A_url + '/D/gamma' + '@4',
                                     Acopy_gamma_path)

  svntest.actions.run_and_verify_svn(None, expected_merge_output(5,
                                     'D    ' + Acopy_gamma_path + '\n'),
                                     [], 'merge', '-r1:5', '--force',
                                     A_url, Acopy_path)

# Test for issue:
#
#   2883: No-op merge (without skip) should not change mergeinfo.
def no_mergeinfo_from_no_op_merge(sbox):
  "no-op merge without skips doesn't change mergeinfo"

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox)

  # Some paths we'll care about
  beta_COPY_path = os.path.join(wc_dir, "A_COPY", "B", "E", "beta")
  G_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G")
  tau_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G", "tau")
  C_COPY_path = os.path.join(wc_dir, "A_COPY", "C")

  # Merge r5 into A_COPY/B/E/beta and commit it.
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_beta_COPY_path = shorten_path_kludge(beta_COPY_path)
  expected_skip = wc.State(short_beta_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5,
                                      'U    ' + short_beta_COPY_path +
                                      '\n'), [], 'merge', '-c5',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)
  os.chdir(saved_cwd)

  expected_output = wc.State(wc_dir,
                             {'A_COPY/B/E/beta' : Item(verb='Sending')})
  wc_status.tweak('A_COPY/B/E/beta', wc_rev=7)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)

  # Merge r4 into A_COPY/D/G.
  short_G_COPY_path = shorten_path_kludge(G_COPY_path)
  expected_output = wc.State(short_G_COPY_path, {
    'rho' : Item(status='U ')
    })
  expected_status = wc.State(short_G_COPY_path, {
    ''         : Item(status=' M', wc_rev=2),
    'pi'       : Item(status='  ', wc_rev=2),
    'rho'      : Item(status='M ', wc_rev=2),
    'tau'      : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G:1,4'}),
    'pi'       : Item("This is the file 'pi'.\n"),
    'rho'      : Item("New content"),
    'tau'      : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State(short_G_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_G_COPY_path, '3', '4',
                                       sbox.repo_url + '/A/D/G',
                                       expected_output, expected_disk,
                                       expected_status, expected_skip,
                                       None, None, None, None, None, 1)
  wc_status.tweak('A_COPY/D/G', status=' M')
  wc_status.tweak('A_COPY/D/G/rho', status='M ')

  # Now try a few no-op variants, in every case no mergeinfo should
  # change from the above state.

  # Do a no-op merge to a file with committed mergeinfo:
  #   -r2:4 into A_COPY/B/E/beta.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(3, None, 4),
                                     [], 'merge', '-r2:4',
                                     sbox.repo_url + '/A/B/E/beta',
                                     short_beta_COPY_path)

  # Do a no-op merge to a file with inherited mergeinfo:
  # -c3 into A_COPY/D/G/tau.
  short_tau_COPY_path = shorten_path_kludge(tau_COPY_path)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(3),
                                     [], 'merge', '-c3',
                                     sbox.repo_url + '/A/D/G/tau',
                                     short_tau_COPY_path)

  # Do a no-op merge to a dir with local mergeinfo:
  #   -r4:6 into A_COPY/D/G.
  short_tau_COPY_path = shorten_path_kludge(tau_COPY_path)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5, None, 6),
                                     [], 'merge', '-r4:6',
                                     sbox.repo_url + '/A/D/G',
                                     short_G_COPY_path)
  os.chdir(saved_cwd)
  svntest.actions.run_and_verify_svn(None, ["/A/D/G:1,4\n"], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     G_COPY_path)

  # Do a no-op merge to a dir with inherited mergeinfo:
  #   All available revs into A_COPY/C.
  os.chdir(svntest.main.work_dir)
  short_C_COPY_path = shorten_path_kludge(C_COPY_path)
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(2, None, 7),
                                     [], 'merge',
                                     sbox.repo_url + '/A/C',
                                     short_C_COPY_path)
  os.chdir(saved_cwd)

  # A final check of the WC's status to ensure nothing unexpected occurred
  # (that the above merge's stdout/stderr didn't already reveal).
  svntest.actions.run_and_verify_status(wc_dir, wc_status)

  # Test for issue #2827
  # Handle merge info for sparsely-populated directories
def merge_to_sparse_directories(sbox):
  "merge to sparse directories"

  # Merges into sparse working copies should set non-inheritable mergeinfo
  # on the deepest directories present in the WC.

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, False, 1)

  # Some paths we'll care about
  A_path = os.path.join(wc_dir, "A")
  D_path = os.path.join(wc_dir, "A", "D")
  I_path = os.path.join(wc_dir, "A", "C", "I")
  G_path = os.path.join(wc_dir, "A", "D", "G")
  A_COPY_path = os.path.join(wc_dir, "A_COPY")

  # Make a few more changes to the merge source...

  # r7 - modify and commit A/mu
  svntest.main.file_write(os.path.join(wc_dir, "A", "mu"),
                          "New content")
  expected_output = wc.State(wc_dir, {'A/mu' : Item(verb='Sending')})
  wc_status.tweak('A/mu', wc_rev=7)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)
  wc_disk.tweak('A/mu', contents="New content")

  # r8 - Add a prop to A/D and commit.
  svntest.actions.run_and_verify_svn(None, ["At revision 7.\n"], [],
                                     'up', wc_dir)
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      D_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', D_path)
  expected_output = svntest.wc.State(wc_dir, {
    'A/D'            : Item(verb='Sending'),
    })
  wc_status.tweak(wc_rev=7)
  wc_status.tweak('A/D', wc_rev=8)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)

  # r9 - Add a prop to A and commit.
  svntest.actions.run_and_verify_svn(None, ["At revision 8.\n"], [],
                                     'up', wc_dir)
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      A_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', A_path)
  expected_output = svntest.wc.State(wc_dir, {
    'A'            : Item(verb='Sending'),
    })
  wc_status.tweak(wc_rev=8)
  wc_status.tweak('A', wc_rev=9)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)

  # Do an --immediates checkout of A_COPY
  immediates_dir = sbox.add_wc_path('immediates')
  expected_output = wc.State(immediates_dir, {
    'B'  : Item(status='A '),
    'mu' : Item(status='A '),
    'C'  : Item(status='A '),
    'D'  : Item(status='A '),
    ''   : Item(status=' U'),
    })
  expected_disk = wc.State('', {
    'B'  : Item(),
    'mu' : Item("This is the file 'mu'.\n"),
    'C'  : Item(),
    'D'  : Item(),
    })
  svntest.actions.run_and_verify_checkout(sbox.repo_url + "/A_COPY",
                                          immediates_dir,
                                          expected_output, expected_disk,
                                          None, None, None, None,
                                          "--depth", "immediates")

  # Merge r4:9 into the immediates WC.
  # The root of the immediates WC should get inheritable r4:9 as should
  # the one file present 'mu'.  The three directory children present, 'B',
  # 'C', and 'D' are checked out at depth empty, so get non-inheritable
  # mergeinfo for r4:9.  The root and 'D' do should also get the changes
  # that affect them directly (the prop adds from r8 and r9).  Any changes
  # deeper than the immediates should be skipped.
  #
  # Search for the comment entitled "The Merge Kluge" elsewhere in
  # this file, to understand why we shorten and chdir() below.
  short_immediates_dir = shorten_path_kludge(immediates_dir)
  expected_output = wc.State(short_immediates_dir, {
    'D'  : Item(status=' U'),
    'mu' : Item(status='U '),
    ''   : Item(status=' U'),
    })
  expected_status = wc.State(short_immediates_dir, {
    ''          : Item(status=' M', wc_rev=9),
    'B'         : Item(status=' M', wc_rev=9),
    'mu'        : Item(status='M ', wc_rev=9),
    'C'         : Item(status=' M', wc_rev=9),
    'D'         : Item(status=' M', wc_rev=9),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,5-9',
                              "prop:name" : "propval"}),
    'B'         : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,5-9*'}),
    'mu'        : Item("New content"),
    'C'         : Item(props={SVN_PROP_MERGE_INFO : '/A/C:1,5-9*'}),
    'D'         : Item(props={SVN_PROP_MERGE_INFO : '/A/D:1,5-9*',
                              "prop:name" : "propval"}),
    })
  expected_skip = wc.State(short_immediates_dir, {})
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_immediates_dir, '4', '9',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Do a --files checkout of A_COPY
  files_dir = sbox.add_wc_path('files')
  expected_output = wc.State(files_dir, {
    'mu' : Item(status='A '),
    ''   : Item(status=' U'),
    })
  expected_disk = wc.State('', {
    'mu' : Item("This is the file 'mu'.\n"),
    })
  svntest.actions.run_and_verify_checkout(sbox.repo_url + "/A_COPY",
                                          files_dir,
                                          expected_output, expected_disk,
                                          None, None, None, None,
                                          "--depth", "files")

  # Merge r4:9 into the files WC.
  # The root of the files WC should get non-inheritable r4:9 and its one
  # present child 'mu' should get the same but inheritable.  The root
  # should also get the change that affects it directly (the prop add
  # from r9).
  short_files_dir = shorten_path_kludge(files_dir)
  expected_output = wc.State(short_files_dir, {
    'mu' : Item(status='U '),
    ''   : Item(status=' U'),
    })
  expected_status = wc.State(short_files_dir, {
    ''          : Item(status=' M', wc_rev=9),
    'mu'        : Item(status='MM', wc_rev=9),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,5-9*',
                              "prop:name" : "propval"}),
    'mu'        : Item("New content",
                       props={SVN_PROP_MERGE_INFO : '/A/mu:1,5-9'}),
    })
  expected_skip = wc.State(short_files_dir, {})
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_files_dir, '4', '9',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Do an --empty checkout of A_COPY
  empty_dir = sbox.add_wc_path('empty')
  expected_output = wc.State(empty_dir, {
    ''   : Item(status=' U'),
    })
  expected_disk = wc.State('', {})
  svntest.actions.run_and_verify_checkout(sbox.repo_url + "/A_COPY",
                                          empty_dir,
                                          expected_output, expected_disk,
                                          None, None, None, None,
                                          "--depth", "empty")

  # Merge r4:9 into the empty WC.
  # The root of the files WC should get non-inheritable r4:9 and also get
  # the one change that affects it directly (the prop add from r9).
  short_empty_dir = shorten_path_kludge(empty_dir)
  expected_output = wc.State(short_empty_dir, {
    ''   : Item(status=' U'),
    })
  expected_status = wc.State(short_empty_dir, {
    ''          : Item(status=' M', wc_rev=9),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A:1,5-9*',
                              "prop:name" : "propval"}),
    })
  expected_skip = wc.State(short_empty_dir, {})
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_empty_dir, '4', '9',
                                       sbox.repo_url + \
                                       '/A',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

def merge_old_and_new_revs_from_renamed_dir(sbox):
  "merge -rold(before rename):head renamed dir"

  ## See http://svn.haxx.se/dev/archive-2007-09/0706.shtml ##

  # Create a WC with a single branch
  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True, 1)

  # Some paths we'll care about
  repo_url = sbox.repo_url
  A_url = repo_url + '/A'
  A_MOVED_url = repo_url + '/A_MOVED'
  A_COPY_path = os.path.join(wc_dir, 'A_COPY')
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  A_MOVED_mu_path = os.path.join(wc_dir, 'A_MOVED', 'mu')

  # Make a modification to A/mu
  svntest.main.file_write(mu_path, "This is the file 'mu' modified.\n")
  expected_output = wc.State(wc_dir, {'A/mu' : Item(verb='Sending')})
  wc_status.add({'A/mu'     : Item(status='  ', wc_rev=3)})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  # Move A to A_MOVED
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 4.\n'],
                                     [], 'mv', '-m', 'mv A to A_MOVED',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url, A_MOVED_url)

  # Update the working copy to get A_MOVED
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Make a modification to A_MOVED/mu
  svntest.main.file_write(A_MOVED_mu_path, "This is 'mu' in A_MOVED.\n")
  expected_output = wc.State(wc_dir, {'A_MOVED/mu' : Item(verb='Sending')})
  expected_status = svntest.actions.get_virginal_state(wc_dir, 4)
  expected_status.remove('A', 'A/mu', 'A/C', 'A/D', 'A/B', 'A/B/lambda',
                         'A/B/E', 'A/B/E/alpha', 'A/B/E/beta', 'A/B/F',
                         'A/D/gamma', 'A/D/G', 'A/D/G/pi', 'A/D/G/rho',
                         'A/D/G/tau', 'A/D/H', 'A/D/H/chi', 'A/D/H/omega',
                         'A/D/H/psi')
  expected_status.add({
    ''                 : Item(status='  ', wc_rev=4),
    'iota'             : Item(status='  ', wc_rev=4),
    'A_MOVED'          : Item(status='  ', wc_rev=4),
    'A_MOVED/mu'       : Item(status='  ', wc_rev=5),
    'A_MOVED/C'        : Item(status='  ', wc_rev=4),
    'A_MOVED/D'        : Item(status='  ', wc_rev=4),
    'A_MOVED/B'        : Item(status='  ', wc_rev=4),
    'A_MOVED/B/lambda' : Item(status='  ', wc_rev=4),
    'A_MOVED/B/E'      : Item(status='  ', wc_rev=4),
    'A_MOVED/B/E/alpha': Item(status='  ', wc_rev=4),
    'A_MOVED/B/E/beta' : Item(status='  ', wc_rev=4),
    'A_MOVED/B/F'      : Item(status='  ', wc_rev=4),
    'A_MOVED/D/gamma'  : Item(status='  ', wc_rev=4),
    'A_MOVED/D/G'      : Item(status='  ', wc_rev=4),
    'A_MOVED/D/G/pi'   : Item(status='  ', wc_rev=4),
    'A_MOVED/D/G/rho'  : Item(status='  ', wc_rev=4),
    'A_MOVED/D/G/tau'  : Item(status='  ', wc_rev=4),
    'A_MOVED/D/H'      : Item(status='  ', wc_rev=4),
    'A_MOVED/D/H/chi'  : Item(status='  ', wc_rev=4),
    'A_MOVED/D/H/omega': Item(status='  ', wc_rev=4),
    'A_MOVED/D/H/psi'  : Item(status='  ', wc_rev=4),
    'A_COPY'           : Item(status='  ', wc_rev=4),
    'A_COPY/mu'        : Item(status='  ', wc_rev=4),
    'A_COPY/C'         : Item(status='  ', wc_rev=4),
    'A_COPY/D'         : Item(status='  ', wc_rev=4),
    'A_COPY/B'         : Item(status='  ', wc_rev=4),
    'A_COPY/B/lambda'  : Item(status='  ', wc_rev=4),
    'A_COPY/B/E'       : Item(status='  ', wc_rev=4),
    'A_COPY/B/E/alpha' : Item(status='  ', wc_rev=4),
    'A_COPY/B/E/beta'  : Item(status='  ', wc_rev=4),
    'A_COPY/B/F'       : Item(status='  ', wc_rev=4),
    'A_COPY/D/gamma'   : Item(status='  ', wc_rev=4),
    'A_COPY/D/G'       : Item(status='  ', wc_rev=4),
    'A_COPY/D/G/pi'    : Item(status='  ', wc_rev=4),
    'A_COPY/D/G/rho'   : Item(status='  ', wc_rev=4),
    'A_COPY/D/G/tau'   : Item(status='  ', wc_rev=4),
    'A_COPY/D/H'       : Item(status='  ', wc_rev=4),
    'A_COPY/D/H/chi'   : Item(status='  ', wc_rev=4),
    'A_COPY/D/H/omega' : Item(status='  ', wc_rev=4),
    'A_COPY/D/H/psi'   : Item(status='  ', wc_rev=4),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  short_A_COPY = shorten_path_kludge(A_COPY_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  # Merge /A_MOVED to /A_COPY
  expected_output = wc.State(short_A_COPY, {
    ''   : Item(status=' G'),
    'mu' : Item(status='U '),
    })
  expected_status = wc.State(short_A_COPY, {
    ''         : Item(status=' M', wc_rev=4),
    'mu'       : Item(status='M ', wc_rev=4),
    'C'        : Item(status='  ', wc_rev=4),
    'D'        : Item(status='  ', wc_rev=4),
    'B'        : Item(status='  ', wc_rev=4),
    'B/lambda' : Item(status='  ', wc_rev=4),
    'B/E'      : Item(status='  ', wc_rev=4),
    'B/E/alpha': Item(status='  ', wc_rev=4),
    'B/E/beta' : Item(status='  ', wc_rev=4),
    'B/F'      : Item(status='  ', wc_rev=4),
    'D/gamma'  : Item(status='  ', wc_rev=4),
    'D/G'      : Item(status='  ', wc_rev=4),
    'D/G/pi'   : Item(status='  ', wc_rev=4),
    'D/G/rho'  : Item(status='  ', wc_rev=4),
    'D/G/tau'  : Item(status='  ', wc_rev=4),
    'D/H'      : Item(status='  ', wc_rev=4),
    'D/H/chi'  : Item(status='  ', wc_rev=4),
    'D/H/omega': Item(status='  ', wc_rev=4),
    'D/H/psi'  : Item(status='  ', wc_rev=4),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A:1-3'}),
    'mu'       : Item("This is 'mu' in A_MOVED.\n"),
    'C'        : Item(),
    'D'        : Item(),
    'B'        : Item(),
    'B/lambda' : Item("This is the file 'lambda'.\n"),
    'B/E'      : Item(),
    'B/E/alpha': Item("This is the file 'alpha'.\n"),
    'B/E/beta' : Item("This is the file 'beta'.\n"),
    'B/F'      : Item(),
    'D/gamma'  : Item("This is the file 'gamma'.\n"),
    'D/G'      : Item(),
    'D/G/pi'   : Item("This is the file 'pi'.\n"),
    'D/G/rho'  : Item("This is the file 'rho'.\n"),
    'D/G/tau'  : Item("This is the file 'tau'.\n"),
    'D/H'      : Item(),
    'D/H/chi'  : Item("This is the file 'chi'.\n"),
    'D/H/omega': Item("This is the file 'omega'.\n"),
    'D/H/psi'  : Item("This is the file 'psi'.\n"),
    })
  expected_skip = wc.State(short_A_COPY, {})
  svntest.actions.run_and_verify_merge(short_A_COPY, '2', '5',
                                       A_MOVED_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

def merge_with_child_having_different_rev_ranges_to_merge(sbox):
  "child having different rev ranges to merge"
  #Modify A/mu to 30 lines with a content 'line1'...'line30' commit it at r2.
  #Create a branch A_COPY from A, commit it at r3.
  #Modify A/mu line number 7 to 'LINE7' modify and commit at r4.
  #Modify A/mu line number 17 to 'LINE17' modify, set prop 'prop1' on 'A' 
  #with a value 'val1' and commit at r5.
  #Modify A/mu line number 27 to 'LINE27' modify and commit at r6.
  #Merge r5 to 'A/mu' as a single file merge explicitly to 'A_COPY/mu'.
  #Merge r3:6 from 'A' to 'A_COPY
  #This should merge r4 and then r5 through r6.
  #Revert r5 and r6 via single file merge on A_COPY/mu.
  #Revert r6 through r4 on A_COPY this should get back us the pristine copy.
  #Merge r3:6 from 'A' to 'A_COPY
  #Revert r5 on A_COPY/mu
  #Modify line number 17 with 'some other line17' of A_COPY/mu
  #Merge r6:3 from 'A' to 'A_COPY, This should leave line number 17
  #undisturbed in A_COPY/mu, rest should be reverted.

  # Create a WC
  sbox.build()
  wc_dir = sbox.wc_dir
  A_path = os.path.join(wc_dir, 'A')
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  repo_url = sbox.repo_url
  A_url = repo_url + '/A'
  A_mu_url = repo_url + '/A/mu'
  A_COPY_url = repo_url + '/A_COPY'
  A_COPY_path = os.path.join(wc_dir, 'A_COPY')
  A_COPY_mu_path = os.path.join(wc_dir, 'A_COPY', 'mu')
  thirty_line_dummy_text = 'line1\n'
  for i in range(2, 31):
    thirty_line_dummy_text += 'line' + str(i) + '\n'

  svntest.main.file_write(mu_path, thirty_line_dummy_text)
  expected_output = wc.State(wc_dir, {'A/mu' : Item(verb='Sending')})
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', wc_rev=2)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'cp', A_url, A_COPY_url, '-m', 'rev 3')
  # Update the working copy to get A_COPY
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)
  expected_status.add({'A_COPY'           : Item(status='  '),
                       'A_COPY/mu'        : Item(status='  '),
                       'A_COPY/C'         : Item(status='  '),
                       'A_COPY/D'         : Item(status='  '),
                       'A_COPY/B'         : Item(status='  '),
                       'A_COPY/B/lambda'  : Item(status='  '),
                       'A_COPY/B/E'       : Item(status='  '),
                       'A_COPY/B/E/alpha' : Item(status='  '),
                       'A_COPY/B/E/beta'  : Item(status='  '),
                       'A_COPY/B/F'       : Item(status='  '),
                       'A_COPY/D/gamma'   : Item(status='  '),
                       'A_COPY/D/G'       : Item(status='  '),
                       'A_COPY/D/G/pi'    : Item(status='  '),
                       'A_COPY/D/G/rho'   : Item(status='  '),
                       'A_COPY/D/G/tau'   : Item(status='  '),
                       'A_COPY/D/H'       : Item(status='  '),
                       'A_COPY/D/H/chi'   : Item(status='  '),
                       'A_COPY/D/H/omega' : Item(status='  '),
                       'A_COPY/D/H/psi'   : Item(status='  ')})
  expected_status.tweak(wc_rev=3)
  tweaked_7th_line = thirty_line_dummy_text.replace('line7', 'LINE 7')
  svntest.main.file_write(mu_path, tweaked_7th_line)
  expected_status.tweak('A/mu', wc_rev=4)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)
  expected_status.tweak(wc_rev=4)
  tweaked_17th_line = tweaked_7th_line.replace('line17', 'LINE 17')
  svntest.main.file_write(mu_path, tweaked_17th_line)
  svntest.main.run_svn(None, 'propset', 'prop1', 'val1', A_path)
  expected_output = wc.State(wc_dir, 
                             {
                              'A'    : Item(verb='Sending'),
                              'A/mu' : Item(verb='Sending')
                             }
                            )
  expected_status.tweak('A', wc_rev=5)
  expected_status.tweak('A/mu', wc_rev=5)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  tweaked_27th_line = tweaked_17th_line.replace('line27', 'LINE 27')
  svntest.main.file_write(mu_path, tweaked_27th_line)
  expected_status.tweak('A/mu', wc_rev=6)
  expected_output = wc.State(wc_dir, {'A/mu' : Item(verb='Sending')})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)
  # Merge r5 to A_COPY/mu
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(5,
                                       ['U    ' + A_COPY_mu_path + '\n']),
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge', '-r4:5',
                                     A_mu_url,
                                     A_COPY_mu_path)

  short_A_COPY = shorten_path_kludge(A_COPY_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  expected_skip = wc.State(short_A_COPY, {})
  expected_output = wc.State(short_A_COPY, {
    ''   : Item(status=' U'),
    'mu' : Item(status='G '),
    })
  expected_status = wc.State(short_A_COPY, {
    ''         : Item(status=' M', wc_rev=4),
    'mu'       : Item(status='M ', wc_rev=4),
    'C'        : Item(status='  ', wc_rev=4),
    'D'        : Item(status='  ', wc_rev=4),
    'B'        : Item(status='  ', wc_rev=4),
    'B/lambda' : Item(status='  ', wc_rev=4),
    'B/E'      : Item(status='  ', wc_rev=4),
    'B/E/alpha': Item(status='  ', wc_rev=4),
    'B/E/beta' : Item(status='  ', wc_rev=4),
    'B/F'      : Item(status='  ', wc_rev=4),
    'D/gamma'  : Item(status='  ', wc_rev=4),
    'D/G'      : Item(status='  ', wc_rev=4),
    'D/G/pi'   : Item(status='  ', wc_rev=4),
    'D/G/rho'  : Item(status='  ', wc_rev=4),
    'D/G/tau'  : Item(status='  ', wc_rev=4),
    'D/H'      : Item(status='  ', wc_rev=4),
    'D/H/chi'  : Item(status='  ', wc_rev=4),
    'D/H/omega': Item(status='  ', wc_rev=4),
    'D/H/psi'  : Item(status='  ', wc_rev=4),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A:1-2,4-6',
                             'prop1' : 'val1'}),
    'mu'       : Item(tweaked_27th_line),
    'C'        : Item(),
    'D'        : Item(),
    'B'        : Item(),
    'B/lambda' : Item("This is the file 'lambda'.\n"),
    'B/E'      : Item(),
    'B/E/alpha': Item("This is the file 'alpha'.\n"),
    'B/E/beta' : Item("This is the file 'beta'.\n"),
    'B/F'      : Item(),
    'D/gamma'  : Item("This is the file 'gamma'.\n"),
    'D/G'      : Item(),
    'D/G/pi'   : Item("This is the file 'pi'.\n"),
    'D/G/rho'  : Item("This is the file 'rho'.\n"),
    'D/G/tau'  : Item("This is the file 'tau'.\n"),
    'D/H'      : Item(),
    'D/H/chi'  : Item("This is the file 'chi'.\n"),
    'D/H/omega': Item("This is the file 'omega'.\n"),
    'D/H/psi'  : Item("This is the file 'psi'.\n"),
    })
  svntest.actions.run_and_verify_merge(short_A_COPY, '3', '6',
                                       A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)
  # Revert r5 and r6 on A_COPY/mu
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(6,
                                       ['G    ' + A_COPY_mu_path + '\n'], 5),
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge', '-r6:4',
                                     A_mu_url,
                                     A_COPY_mu_path)

  expected_output = wc.State(short_A_COPY, {
    ''   : Item(status=' G'),
    'mu' : Item(status='G '),
    })

  expected_status.tweak('', 'mu', status='  ')
  expected_disk.tweak('', props={SVN_PROP_MERGE_INFO : '/A:1-2'})
  expected_disk.tweak('mu', contents=thirty_line_dummy_text)
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY, '6', '3',
                                       A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)
  expected_disk.tweak('', props={SVN_PROP_MERGE_INFO : '/A:1-2,4-6',
                                 'prop1' : 'val1'})
  expected_disk.tweak('mu', contents=tweaked_27th_line)
  expected_output = wc.State(short_A_COPY, {
    ''   : Item(status=' U'),
    'mu' : Item(status='U '),
    })
  expected_status.tweak('', status=' M')
  expected_status.tweak('mu', status='M ')
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY, '3', '6',
                                       A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)
  #Revert r5 on A_COPY/mu
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(-5,
                                       ['G    ' + A_COPY_mu_path + '\n']),
                                     [],
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     'merge', '-r5:4',
                                     A_mu_url,
                                     A_COPY_mu_path)
  tweaked_17th_line_1 = tweaked_27th_line.replace('LINE 17', 
                                                  'some other line17')
  tweaked_17th_line_2 = thirty_line_dummy_text.replace('line17',
                                                       'some other line17')
  svntest.main.file_write(A_COPY_mu_path, tweaked_17th_line_1)
  expected_output = wc.State(short_A_COPY, {
    ''   : Item(status=' G'),
    'mu' : Item(status='G '),
    })
  expected_status.tweak('', status='  ')
  expected_status.tweak('mu', status='M ')
  expected_disk.tweak('', props={SVN_PROP_MERGE_INFO : '/A:1-2'})
  expected_disk.tweak('mu', contents=tweaked_17th_line_2)
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_A_COPY, '6', '3',
                                       A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None, 1)
  os.chdir(saved_cwd)

def merge_old_and_new_revs_from_renamed_file(sbox):
  "merge -rold(before rename):head renamed file"

  ## See http://svn.haxx.se/dev/archive-2007-09/0706.shtml ##

  # Create a WC
  sbox.build()
  wc_dir = sbox.wc_dir

  # Some paths we'll care about
  repo_url = sbox.repo_url
  mu_url = repo_url + '/A/mu'
  mu_MOVED_url = repo_url + '/A/mu_MOVED'
  mu_COPY_url = repo_url + '/A/mu_COPY'
  mu_COPY_path = os.path.join(wc_dir, 'A', 'mu_COPY')
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  mu_MOVED_path = os.path.join(wc_dir, 'A', 'mu_MOVED')

  # Copy mu to mu_COPY
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 2.\n'],
                                     [], 'cp', '-m', 'cp mu to mu_COPY',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     mu_url, mu_COPY_url)

  # Make a modification to A/mu
  svntest.main.file_write(mu_path, "This is the file 'mu' modified.\n")
  expected_output = wc.State(wc_dir, {'A/mu' : Item(verb='Sending')})
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', wc_rev=3)
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  # Move mu to mu_MOVED
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 4.\n'],
                                     [], 'mv', '-m', 'mv mu to mu_MOVED',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     mu_url, mu_MOVED_url)

  # Update the working copy to get mu_MOVED
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  # Make a modification to mu_MOVED
  svntest.main.file_write(mu_MOVED_path, "This is 'mu' in mu_MOVED.\n")
  expected_output = wc.State(wc_dir, {'A/mu_MOVED' : Item(verb='Sending')})
  expected_status = svntest.actions.get_virginal_state(wc_dir, 4)
  expected_status.remove('A/mu')
  expected_status.add({
    'A/mu_MOVED'  : Item(status='  ', wc_rev=5),
    'A/mu_COPY'   : Item(status='  ', wc_rev=4),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  # Merge A/mu_MOVED to A/mu_COPY
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output(None, 'UG   '+
                                                           mu_COPY_path +
                                                           '\n'),
                                     [], 'merge', '-r', '1:5',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     mu_MOVED_url,
                                     mu_COPY_path)


def merge_with_auto_rev_range_detection(sbox):
  "merge with auto detection of revision ranges"

  # Merge using '-g' option causes merge failure post r26803.
  ## See Also http://svn.haxx.se/dev/archive-2007-09/0735.shtml ##

  # Create a WC
  sbox.build()
  wc_dir = sbox.wc_dir

  # Some paths we'll care about
  repo_url = sbox.repo_url
  A_url = repo_url + '/A'
  A_COPY_url = repo_url + '/A_COPY'
  B1_path = os.path.join(wc_dir, 'A', 'B1')
  B1_mu_path = os.path.join(wc_dir, 'A', 'B1', 'mu')
  A_COPY_path = os.path.join(wc_dir, 'A_COPY')

  # Create B1 inside A
  svntest.actions.run_and_verify_svn(None, ["A         " + B1_path + "\n"],
                                     [], 'mkdir',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     B1_path)

  # Add a file mu inside B1
  svntest.main.file_write(B1_mu_path, "This is the file 'mu'.\n")
  svntest.actions.run_and_verify_svn(None, ["A         " + B1_mu_path + "\n"],
                                     [], 'add', B1_mu_path)

  # Commit B1 and B1/mu
  expected_output = wc.State(wc_dir, {
    'A/B1'      : Item(verb='Adding'),
    'A/B1/mu'   : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B1'      : Item(status='  ', wc_rev=2),
    'A/B1/mu'   : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  # Copy A to A_COPY
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 3.\n'],
                                     [], 'cp', '-m', 'cp A to A_COPY',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_url, A_COPY_url)

  # Make a modification to A/B1/mu
  svntest.main.file_write(B1_mu_path, "This is the file 'mu' modified.\n")
  expected_output = wc.State(wc_dir, {'A/B1/mu' : Item(verb='Sending')})
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B1'      : Item(status='  ', wc_rev=2),
    'A/B1/mu'   : Item(status='  ', wc_rev=4),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None, None, None,
                                        None, None, wc_dir)

  # Update the working copy to get A_COPY
  svntest.actions.run_and_verify_svn(None, None, [], 'up', wc_dir)

  short_A_COPY = shorten_path_kludge(A_COPY_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  # Merge /A to /A_COPY
  expected_output = wc.State(short_A_COPY, {
    'B1/mu' : Item(status='U '),
    })
  expected_status = wc.State(short_A_COPY, {
    ''         : Item(status=' M', wc_rev=4),
    'mu'       : Item(status='  ', wc_rev=4),
    'C'        : Item(status='  ', wc_rev=4),
    'D'        : Item(status='  ', wc_rev=4),
    'B'        : Item(status='  ', wc_rev=4),
    'B/lambda' : Item(status='  ', wc_rev=4),
    'B/E'      : Item(status='  ', wc_rev=4),
    'B/E/alpha': Item(status='  ', wc_rev=4),
    'B/E/beta' : Item(status='  ', wc_rev=4),
    'B/F'      : Item(status='  ', wc_rev=4),
    'B1'       : Item(status='  ', wc_rev=4),
    'B1/mu'    : Item(status='M ', wc_rev=4),
    'D/gamma'  : Item(status='  ', wc_rev=4),
    'D/G'      : Item(status='  ', wc_rev=4),
    'D/G/pi'   : Item(status='  ', wc_rev=4),
    'D/G/rho'  : Item(status='  ', wc_rev=4),
    'D/G/tau'  : Item(status='  ', wc_rev=4),
    'D/H'      : Item(status='  ', wc_rev=4),
    'D/H/chi'  : Item(status='  ', wc_rev=4),
    'D/H/omega': Item(status='  ', wc_rev=4),
    'D/H/psi'  : Item(status='  ', wc_rev=4),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A:1-4'}),
    'mu'       : Item("This is the file 'mu'.\n"),
    'C'        : Item(),
    'D'        : Item(),
    'B'        : Item(),
    'B/lambda' : Item("This is the file 'lambda'.\n"),
    'B/E'      : Item(),
    'B/E/alpha': Item("This is the file 'alpha'.\n"),
    'B/E/beta' : Item("This is the file 'beta'.\n"),
    'B/F'      : Item(),
    'B1'       : Item(),
    'B1/mu'    : Item("This is the file 'mu' modified.\n"),
    'D/gamma'  : Item("This is the file 'gamma'.\n"),
    'D/G'      : Item(),
    'D/G/pi'   : Item("This is the file 'pi'.\n"),
    'D/G/rho'  : Item("This is the file 'rho'.\n"),
    'D/G/tau'  : Item("This is the file 'tau'.\n"),
    'D/H'      : Item(),
    'D/H/chi'  : Item("This is the file 'chi'.\n"),
    'D/H/omega': Item("This is the file 'omega'.\n"),
    'D/H/psi'  : Item("This is the file 'psi'.\n"),
    })
  expected_skip = wc.State(short_A_COPY, {})
  svntest.actions.run_and_verify_merge(short_A_COPY, None, None,
                                       A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 1, '-g')
  os.chdir(saved_cwd)

def mergeinfo_recording_in_skipped_merge(sbox):
  "mergeinfo recording in skipped merge"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2829. ##

  # Create a WC with a single branch
  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True, 1)

  # Some paths we'll care about
  repo_url = sbox.repo_url
  A_url = repo_url + '/A'
  A_COPY_path = os.path.join(wc_dir, 'A_COPY')
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  alpha_path = os.path.join(wc_dir, 'A', 'B', 'E', 'alpha')
  A_COPY_B_E_path = os.path.join(wc_dir, 'A_COPY', 'B', 'E')
  A_COPY_alpha_path = os.path.join(wc_dir, 'A_COPY', 'B', 'E', 'alpha')
  A_COPY_beta_path = os.path.join(wc_dir, 'A_COPY', 'B', 'E', 'beta')

  # Make a modification to A/mu
  svntest.main.file_write(mu_path, "This is the file 'mu' modified.\n")
  expected_output = wc.State(wc_dir, {'A/mu' : Item(verb='Sending')})
  wc_status.add({'A/mu'     : Item(status='  ', wc_rev=3)})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  # Make a modification to A/B/E/alpha
  svntest.main.file_write(alpha_path, "This is the file 'alpha' modified.\n")
  expected_output = wc.State(wc_dir, {'A/B/E/alpha' : Item(verb='Sending')})
  wc_status.add({'A/B/E/alpha'     : Item(status='  ', wc_rev=4)})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  # Delete A_COPY/B/E
  svntest.actions.run_and_verify_svn(None, None, [], 'rm',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     A_COPY_B_E_path)

  short_A_COPY = shorten_path_kludge(A_COPY_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  # Merge /A to /A_COPY ie., r1 to r4
  expected_output = wc.State(short_A_COPY, {
    'mu' : Item(status='U '),
    })
  expected_status = wc.State(short_A_COPY, {
    ''         : Item(status=' M', wc_rev=2),
    'mu'       : Item(status='M ', wc_rev=2),
    'B'        : Item(status='  ', wc_rev=2),
    'B/lambda' : Item(status='  ', wc_rev=2),
    'B/F'      : Item(status='  ', wc_rev=2),
    'B/E'      : Item(status='D ', wc_rev=2),
    'B/E/alpha': Item(status='D ', wc_rev=2),
    'B/E/beta' : Item(status='D ', wc_rev=2),
    'C'        : Item(status='  ', wc_rev=2),
    'D'        : Item(status='  ', wc_rev=2),
    'D/gamma'  : Item(status='  ', wc_rev=2),
    'D/G'      : Item(status='  ', wc_rev=2),
    'D/G/pi'   : Item(status='  ', wc_rev=2),
    'D/G/rho'  : Item(status='  ', wc_rev=2),
    'D/G/tau'  : Item(status='  ', wc_rev=2),
    'D/H'      : Item(status='  ', wc_rev=2),
    'D/H/chi'  : Item(status='  ', wc_rev=2),
    'D/H/omega': Item(status='  ', wc_rev=2),
    'D/H/psi'  : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''         : Item(props={SVN_PROP_MERGE_INFO : '/A:1-4'}),
    'mu'       : Item("This is the file 'mu' modified.\n"),
    'C'        : Item(),
    'D'        : Item(),
    'B'        : Item(props={SVN_PROP_MERGE_INFO : '/A:1'}),
    'B/lambda' : Item(contents="This is the file 'lambda'.\n",
                      props={SVN_PROP_MERGE_INFO : '/A/B/lambda:1-4'}),
    'B/F'      : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:1-4'}),
    'D/gamma'  : Item("This is the file 'gamma'.\n"),
    'D/G'      : Item(),
    'D/G/pi'   : Item("This is the file 'pi'.\n"),
    'D/G/rho'  : Item("This is the file 'rho'.\n"),
    'D/G/tau'  : Item("This is the file 'tau'.\n"),
    'D/H'      : Item(),
    'D/H/chi'  : Item("This is the file 'chi'.\n"),
    'D/H/omega': Item("This is the file 'omega'.\n"),
    'D/H/psi'  : Item("This is the file 'psi'.\n"),
    })
  expected_skip = wc.State(short_A_COPY, {
    'B/E/alpha' : Item(),
    })
  svntest.actions.run_and_verify_merge(short_A_COPY, None, None,
                                       A_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 1, '-g')

  os.chdir(saved_cwd)

# Test for issue 2818: Provide a 'merge' API which allows for merging of
# arbitrary revision ranges (e.g. '-c 3,5,7')
def cherry_picking(sbox):
  "command line supports cherry picked merge ranges"

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox)

  # Some paths we'll care about
  H_path = os.path.join(wc_dir, "A", "D", "H")
  G_path = os.path.join(wc_dir, "A", "D", "G")
  A_COPY_path = os.path.join(wc_dir, "A_COPY")
  D_COPY_path = os.path.join(wc_dir, "A_COPY", "D")
  G_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G")
  H_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "H")
  rho_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G", "rho")
  omega_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "H", "omega")
  psi_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "H", "psi")

  # Update working copy
  expected_output = svntest.wc.State(wc_dir, {})
  wc_status.tweak(wc_rev='6')
  svntest.actions.run_and_verify_update(wc_dir, expected_output,
                                        wc_disk, wc_status,
                                        None, None, None, None, None, True)

  # Make some prop changes to some dirs.
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      G_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', G_path)
  expected_output = svntest.wc.State(wc_dir, {'A/D/G': Item(verb='Sending'),})
  wc_status.tweak('A/D/G', wc_rev=7)
  wc_disk.tweak('A/D/G', props={'prop:name' : 'propval'})
  
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      H_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', H_path)
  expected_output = svntest.wc.State(wc_dir, {'A/D/H': Item(verb='Sending'),})
  wc_status.tweak('A/D/H', wc_rev=8)
  wc_disk.tweak('A/D/H', props={'prop:name' : 'propval'})
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)

  # Try some merges with multiple ranges that should compact to
  # nothing and result in a no-op.
  svntest.actions.run_and_verify_svn(None, [], [], 'merge',
                                     '-c4', '-c-4',
                                     sbox.repo_url + '/A', A_COPY_path)
  svntest.actions.run_and_verify_svn(None, [], [], 'merge',
                                     '-r7:3', '-r3:7',
                                     sbox.repo_url + '/A', A_COPY_path)
  svntest.actions.run_and_verify_svn(None, [], [], 'merge',
                                     '-r3:2', '-c-4', '-r2:8', '-c-6',
                                     '-r8:3', '-c-3', '-r4:5',
                                     sbox.repo_url + '/A', A_COPY_path)

  # Do multiple additive merges to a file"
  # Merge -r2:4 -c6 into A_COPY/D/G/rho.
  short_rho_COPY_path = shorten_path_kludge(rho_COPY_path)
  expected_skip = wc.State(short_rho_COPY_path, { })
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)
  # run_and_verify_merge doesn't support merging to a file WCPATH
  # so use run_and_verify_svn.
  svntest.actions.run_and_verify_svn(None,
                                     expected_merge_output3(
                                       [[3,4],[6]],
                                       'U    ' + short_rho_COPY_path + '\n'),
                                     [], 'merge', '-r2:4', '-c6',
                                     sbox.repo_url + '/A/D/G/rho',
                                     short_rho_COPY_path)
  os.chdir(saved_cwd)

  # Check rho's status and props.
  expected_status = wc.State(rho_COPY_path,
                             {'' : Item(status='MM', wc_rev=6)})
  svntest.actions.run_and_verify_status(rho_COPY_path, expected_status)
  svntest.actions.run_and_verify_svn(None, ["/A/D/G/rho:1,3-4,6\n"], [],
                                     'propget', SVN_PROP_MERGE_INFO,
                                     rho_COPY_path)

  #Do  multiple additive merges to a directory:
  # Merge -c6 -c8 into A_COPY/D/H
  short_H_COPY_path = shorten_path_kludge(H_COPY_path)
  short_omega_COPY_path = shorten_path_kludge(omega_COPY_path)
  expected_output = expected_merge_output3(
    [[6],[8]],
    ['U    ' + short_omega_COPY_path + '\n',
     ' U   ' + short_H_COPY_path + '\n'])
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None, expected_output,
                                     [], 'merge', '-c6', '-c8',
                                     sbox.repo_url + '/A/D/H',
                                     short_H_COPY_path)
  os.chdir(saved_cwd)

  # Check A_COPY/D/H's status and props.
  expected_status = wc.State(H_COPY_path,
                             {''     : Item(status=' M', wc_rev=6),
                              'psi'  : Item(status='  ', wc_rev=6),
                              'chi'  : Item(status='  ', wc_rev=6),
                              'omega': Item(status='M ', wc_rev=6),})
  svntest.actions.run_and_verify_status(H_COPY_path, expected_status)
  svntest.actions.run_and_verify_svn(None,
                                     [H_COPY_path + " - /A/D/H:1,6,8\n"],
                                     [], 'propget', '-R', SVN_PROP_MERGE_INFO,
                                     H_COPY_path)

  # Do multiple reverse merges to a directory:
  # Merge -c-6 -c-3 into A_COPY
  short_A_COPY_path = shorten_path_kludge(A_COPY_path)
  short_omega_COPY_path = shorten_path_kludge(omega_COPY_path)
  expected_output = expected_merge_output3(
    [[-3],[-6]], 'G    ' + short_omega_COPY_path + '\n')
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None, expected_output,
                                     [], 'merge', '-c-3', '-c-6',
                                     sbox.repo_url + '/A',
                                     short_A_COPY_path)
  os.chdir(saved_cwd)
  expected_status = wc.State(A_COPY_path,
                             {''          : Item(status='  ', wc_rev=6),
                              'B'         : Item(status='  ', wc_rev=6),
                              'B/lambda'  : Item(status='  ', wc_rev=6),
                              'B/E'       : Item(status='  ', wc_rev=6),
                              'B/E/alpha' : Item(status='  ', wc_rev=6),
                              'B/E/beta'  : Item(status='  ', wc_rev=6),
                              'B/F'       : Item(status='  ', wc_rev=6),
                              'mu'        : Item(status='  ', wc_rev=6),
                              'C'         : Item(status='  ', wc_rev=6),
                              'D'         : Item(status='  ', wc_rev=6),
                              'D/gamma'   : Item(status='  ', wc_rev=6),
                              'D/G'       : Item(status='  ', wc_rev=6),
                              'D/G/pi'    : Item(status='  ', wc_rev=6),
                              'D/G/rho'   : Item(status='MM', wc_rev=6),
                              'D/G/tau'   : Item(status='  ', wc_rev=6),
                              'D/H'       : Item(status=' M', wc_rev=6),
                              'D/H/chi'   : Item(status='  ', wc_rev=6),
                              'D/H/psi'   : Item(status='  ', wc_rev=6),
                              'D/H/omega' : Item(status='  ', wc_rev=6),})
  svntest.actions.run_and_verify_status(A_COPY_path, expected_status)
  expected_out = A_COPY_path + " - /A:1\n|" + \
    H_COPY_path +  " - /A/D/H:1,8\n|" + \
    rho_COPY_path + " - /A/D/G/rho:1,4\n"
  # Construct proper regex for '\' infested Windows paths.
  if sys.platform == 'win32':
    expected_out = expected_out.replace("\\", "\\\\")
  svntest.actions.run_and_verify_svn(None, expected_out, [],
                                     'propget', '-R', SVN_PROP_MERGE_INFO,
                                     A_COPY_path)

  # Do both additive and reverse merges to a directory:
  # Merge -r2:3 -c-4 -r4:7 to A_COPY/D
  short_D_COPY_path = shorten_path_kludge(D_COPY_path)
  short_psi_COPY_path = shorten_path_kludge(psi_COPY_path)
  short_G_COPY_path = shorten_path_kludge(G_COPY_path)
  expected_output = expected_merge_output3(
    [[3],[-4], [5,7]],
    [' U   ' + short_G_COPY_path     + '\n',
     'U    ' + short_omega_COPY_path + '\n',
     'U    ' + short_psi_COPY_path   + '\n',
     'G    ' + short_rho_COPY_path   + '\n'])

  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_svn(None, expected_output, [], 'merge',
                                     '-r2:3', '-c-4', '-r4:7',
                                     sbox.repo_url + '/A/D',
                                     short_D_COPY_path)
  os.chdir(saved_cwd)
  expected_status = wc.State(D_COPY_path,
                             {''        : Item(status=' M', wc_rev=6),
                              'gamma'   : Item(status='  ', wc_rev=6),
                              'G'       : Item(status=' M', wc_rev=6),
                              'G/pi'    : Item(status='  ', wc_rev=6),
                              'G/rho'   : Item(status='  ', wc_rev=6),
                              'G/tau'   : Item(status='  ', wc_rev=6),
                              'H'       : Item(status=' M', wc_rev=6),
                              'H/chi'   : Item(status='  ', wc_rev=6),
                              'H/psi'   : Item(status='M ', wc_rev=6),
                              'H/omega' : Item(status='M ', wc_rev=6),})
  svntest.actions.run_and_verify_status(D_COPY_path, expected_status)
  expected_out = D_COPY_path + " - /A/D:1,3,5-7\n|" + \
    H_COPY_path +  " - /A/D/H:1,3,5-8\n"
  # Construct proper regex for '\' infested Windows paths.
  if sys.platform == 'win32':
    expected_out = expected_out.replace("\\", "\\\\")
  svntest.actions.run_and_verify_svn(None, expected_out, [],
                                     'propget', '-R', SVN_PROP_MERGE_INFO,
                                     D_COPY_path)

def propchange_of_subdir_raises_conflict(sbox):
  "merge of propchange on subdir raises conflict"
  
  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2969. ##

  # Create a WC with a single branch
  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True, 1)

  # Some paths we'll care about
  repo_url = sbox.repo_url
  B_url = repo_url + '/A/B'
  E_path = os.path.join(wc_dir, 'A', 'B', 'E')
  lambda_path = os.path.join(wc_dir, 'A', 'B', 'lambda')
  A_COPY_B_path = os.path.join(wc_dir, 'A_COPY', 'B')
  A_COPY_B_E_path = os.path.join(wc_dir, 'A_COPY', 'B', 'E')
  A_COPY_lambda_path = os.path.join(wc_dir, 'A_COPY', 'B', 'E', 'lambda')

  # Set a property on A/B/E and Make a modification to A/B/lambda
  svntest.main.run_svn(None, 'propset', 'x', 'x', E_path)

  svntest.main.file_write(lambda_path, "This is the file 'lambda' modified.\n")
  expected_output = wc.State(wc_dir, {
    'A/B/lambda' : Item(verb='Sending'),
    'A/B/E'      : Item(verb='Sending'),
    })
  wc_status.add({
    'A/B/lambda'     : Item(status='  ', wc_rev=3),
    'A/B/E'          : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  short_A_COPY_B = shorten_path_kludge(A_COPY_B_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  # Merge /A/B to /A_COPY/B ie., r1 to r3 with depth files
  expected_output = wc.State(short_A_COPY_B, {
    'lambda' : Item(status='U '),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1,2-3*'}),
    'lambda'  : Item(contents="This is the file 'lambda' modified.\n",
                     props={SVN_PROP_MERGE_INFO : '/A/B/lambda:1-3'}),
    'F'       : Item(),
    'E'       : Item(),
    'E/alpha' : Item(contents="This is the file 'alpha'.\n"),
    'E/beta'  : Item(contents="This is the file 'beta'.\n"),
    })
  expected_status = wc.State(short_A_COPY_B, {
    ''         : Item(status=' M', wc_rev=2),
    'lambda'   : Item(status='MM', wc_rev=2),
    'F'        : Item(status='  ', wc_rev=2),
    'E'        : Item(status='  ', wc_rev=2),
    'E/alpha'  : Item(status='  ', wc_rev=2),
    'E/beta'   : Item(status='  ', wc_rev=2),
    })
  expected_skip = wc.State(short_A_COPY_B, {})

  svntest.actions.run_and_verify_merge(short_A_COPY_B, None, None,
                                       B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 1, '-g', '--depth', 'files')

  # Merge /A/B to /A_COPY/B ie., r1 to r3 with infinite depth
  expected_output = wc.State(short_A_COPY_B, {
    'E'       : Item(status=' U'),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-3'}),
    'lambda'  : Item(contents="This is the file 'lambda' modified.\n"),
    'F'       : Item(),
    'E'       : Item(props={'x': 'x'}),
    'E/alpha' : Item(contents="This is the file 'alpha'.\n"),
    'E/beta'  : Item(contents="This is the file 'beta'.\n"),
    })
  expected_status = wc.State(short_A_COPY_B, {
    ''         : Item(status=' M', wc_rev=2),
    'lambda'   : Item(status='M ', wc_rev=2),
    'F'        : Item(status='  ', wc_rev=2),
    'E'        : Item(status=' M', wc_rev=2),
    'E/alpha'  : Item(status='  ', wc_rev=2),
    'E/beta'   : Item(status='  ', wc_rev=2),
    })

  svntest.actions.run_and_verify_merge(short_A_COPY_B, None, None,
                                       B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 1, '-g')

  os.chdir(saved_cwd)

# Test for issue #2971: Reverse merge of prop add segfaults if
# merging to parent of first merge
def reverse_merge_prop_add_on_child(sbox):
  "reverse merge of prop add on child"

  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True, 1)

  # Some paths we'll care about
  G_path = os.path.join(wc_dir, "A", "D", "G")
  D_COPY_path = os.path.join(wc_dir, "A_COPY", "D")
  G_COPY_path = os.path.join(wc_dir, "A_COPY", "D", "G")

  # Make some prop changes to some dirs.
  svntest.actions.run_and_verify_svn(None,
                                     ["property 'prop:name' set on '" +
                                      G_path + "'\n"], [], 'ps',
                                     'prop:name', 'propval', G_path)
  expected_output = svntest.wc.State(wc_dir, {'A/D/G': Item(verb='Sending'),})
  wc_status.tweak('A/D/G', wc_rev=3)
  wc_disk.tweak('A/D/G', props={'prop:name' : 'propval'})
  
  svntest.actions.run_and_verify_commit(wc_dir, expected_output, wc_status,
                                        None, None, None, None, None, wc_dir)

  # Merge -c3's prop add to A_COPY/D/G
  saved_cwd = os.getcwd()
  short_G_COPY_path = shorten_path_kludge(G_COPY_path)
  expected_output = wc.State(short_G_COPY_path, {
    '' : Item(status=' U')
    })
  expected_status = wc.State(short_G_COPY_path, {
    ''    : Item(status=' M', wc_rev=2),
    'pi'  : Item(status='  ', wc_rev=2),
    'rho' : Item(status='  ', wc_rev=2),
    'tau' : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    ''    : Item(props={SVN_PROP_MERGE_INFO : '/A/D/G:1,3',
                        'prop:name' : 'propval'}),
    'pi'  : Item("This is the file 'pi'.\n"),
    'rho' : Item("This is the file 'rho'.\n"),
    'tau' : Item("This is the file 'tau'.\n"),
    })
  expected_skip = wc.State(short_G_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_G_COPY_path, '2', '3',
                                       sbox.repo_url + \
                                       '/A/D/G',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

  # Now merge -c-3 but target the previous target's parent instead.
  short_D_COPY_path = shorten_path_kludge(D_COPY_path)
  expected_output = wc.State(short_D_COPY_path, {
    'G' : Item(status=' G'),
    })
  expected_status = wc.State(short_D_COPY_path, {
    ''        : Item(status='  ', wc_rev=2),
    'G'       : Item(status='  ', wc_rev=2),
    'G/pi'    : Item(status='  ', wc_rev=2),
    'G/rho'   : Item(status='  ', wc_rev=2),
    'G/tau'   : Item(status='  ', wc_rev=2),
    'H'       : Item(status='  ', wc_rev=2),
    'H/chi'   : Item(status='  ', wc_rev=2),
    'H/psi'   : Item(status='  ', wc_rev=2),
    'H/omega' : Item(status='  ', wc_rev=2),
    'gamma'   : Item(status='  ', wc_rev=2),
    })
  expected_disk = wc.State('', {
    'G'       : Item(),
    'G/pi'    : Item("This is the file 'pi'.\n"),
    'G/rho'   : Item("This is the file 'rho'.\n"),
    'G/tau'   : Item("This is the file 'tau'.\n"),
    'H'       : Item(),
    'H/chi'   : Item("This is the file 'chi'.\n"),
    'H/psi'   : Item("This is the file 'psi'.\n"),
    'H/omega' : Item("This is the file 'omega'.\n"),
    'gamma'   : Item("This is the file 'gamma'.\n")
    })
  expected_skip = wc.State(short_D_COPY_path, { })
  os.chdir(svntest.main.work_dir)
  svntest.actions.run_and_verify_merge(short_D_COPY_path, '3', '2',
                                       sbox.repo_url + \
                                       '/A/D',
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None,
                                       None, 1)
  os.chdir(saved_cwd)

def merge_target_with_non_inheritable_mergeinfo(sbox):
  "merge target with non inheritable mergeinfo"
  
  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2970. ##

  # Create a WC with a single branch
  sbox.build()
  wc_dir = sbox.wc_dir
  wc_disk, wc_status = setup_branch(sbox, True, 1)

  # Some paths we'll care about
  repo_url = sbox.repo_url
  B_url = repo_url + '/A/B'
  lambda_path = os.path.join(wc_dir, 'A', 'B', 'lambda')
  newfile_path = os.path.join(wc_dir, 'A', 'B', 'E', 'newfile')
  A_COPY_B_path = os.path.join(wc_dir, 'A_COPY', 'B')

  # Make a modifications to A/B/lambda and add A/B/E/newfile
  svntest.main.file_write(lambda_path, "This is the file 'lambda' modified.\n")
  svntest.main.file_write(newfile_path, "This is the file 'newfile'.\n")
  svntest.actions.run_and_verify_svn(None, None, [], 'add', newfile_path)
  expected_output = wc.State(wc_dir, {
    'A/B/lambda'    : Item(verb='Sending'),
    'A/B/E/newfile' : Item(verb='Adding'),
    })
  wc_status.add({
    'A/B/lambda'     : Item(status='  ', wc_rev=3),
    'A/B/E/newfile'  : Item(status='  ', wc_rev=3),
    })
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        wc_status, None, None, None,
                                        None, None, wc_dir)

  short_A_COPY_B = shorten_path_kludge(A_COPY_B_path)
  saved_cwd = os.getcwd()
  os.chdir(svntest.main.work_dir)

  # Merge /A/B to /A_COPY/B ie., r1 to r3 with depth immediates
  expected_output = wc.State(short_A_COPY_B, {
    'lambda' : Item(status='U '),
    })
  expected_disk = wc.State('', {
    ''        : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-3'}),
    'lambda'  : Item(contents="This is the file 'lambda' modified.\n"),
    'F'       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/F:1,2-3*'}),
    'E'       : Item(props={SVN_PROP_MERGE_INFO : '/A/B/E:1,2-3*'}),
    'E/alpha' : Item(contents="This is the file 'alpha'.\n"),
    'E/beta'  : Item(contents="This is the file 'beta'.\n"),
    })
  expected_status = wc.State(short_A_COPY_B, {
    ''         : Item(status=' M', wc_rev=2),
    'lambda'   : Item(status='M ', wc_rev=2),
    'F'        : Item(status=' M', wc_rev=2),
    'E'        : Item(status=' M', wc_rev=2),
    'E/alpha'  : Item(status='  ', wc_rev=2),
    'E/beta'   : Item(status='  ', wc_rev=2),
    })
  expected_skip = wc.State(short_A_COPY_B, {})

  svntest.actions.run_and_verify_merge(short_A_COPY_B, None, None,
                                       B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 1, '-g', '--depth', 'immediates')

  # Merge /A/B to /A_COPY/B ie., r1 to r3 with infinite depth
  expected_output = wc.State(short_A_COPY_B, {
    'E/newfile'     : Item(status='A '),
    })
  expected_disk = wc.State('', {
    ''          : Item(props={SVN_PROP_MERGE_INFO : '/A/B:1-3'}),
    'lambda'    : Item(contents="This is the file 'lambda' modified.\n"),
    'F'         : Item(),
    'E'         : Item(),
    'E/alpha'   : Item(contents="This is the file 'alpha'.\n"),
    'E/beta'    : Item(contents="This is the file 'beta'.\n"),
    'E/newfile' : Item(contents="This is the file 'newfile'.\n"),
    })
  expected_status = wc.State(short_A_COPY_B, {
    ''            : Item(status=' M', wc_rev=2),
    'lambda'      : Item(status='M ', wc_rev=2),
    'F'           : Item(status='  ', wc_rev=2),
    'E'           : Item(status='  ', wc_rev=2),
    'E/alpha'     : Item(status='  ', wc_rev=2),
    'E/beta'      : Item(status='  ', wc_rev=2),
    'E/newfile'   : Item(status='A ', wc_rev=2),
    })

  svntest.actions.run_and_verify_merge(short_A_COPY_B, None, None,
                                       B_url,
                                       expected_output,
                                       expected_disk,
                                       expected_status,
                                       expected_skip,
                                       None, None, None, None, None,
                                       1, 1, '-g')

  os.chdir(saved_cwd)

########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              textual_merges_galore,
              add_with_history,
              delete_file_and_dir,
              simple_property_merges,
              merge_with_implicit_target_using_r,
              merge_with_implicit_target_using_c,
              merge_with_implicit_target_and_revs,
              merge_catches_nonexistent_target,
              merge_tree_deleted_in_target,
              merge_similar_unrelated_trees,
              merge_with_prev,
              merge_binary_file,
              three_way_merge_add_of_existing_binary_file,
              merge_one_file_using_r,
              merge_one_file_using_c,
              merge_one_file_using_implicit_revs,
              merge_record_only,
              merge_in_new_file_and_diff,
              merge_skips_obstructions,
              merge_into_missing,
              dry_run_adds_file_with_prop,
              merge_binary_with_common_ancestry,
              merge_funny_chars_on_path,
              merge_keyword_expansions,
              merge_prop_change_to_deleted_target,
              merge_file_with_space_in_its_name,
              merge_dir_branches,
              safe_property_merge,
              property_merge_from_branch,
              property_merge_undo_redo,
              cherry_pick_text_conflict,
              merge_file_replace,
              merge_dir_replace,
              XFail(merge_dir_and_file_replace),
              merge_file_replace_to_mixed_rev_wc,
              merge_added_dir_to_deleted_in_target,
              merge_ignore_whitespace,
              merge_ignore_eolstyle,
              merge_add_over_versioned_file_conflicts,
              merge_conflict_markers_matching_eol,
              merge_eolstyle_handling,
              avoid_repeated_merge_using_inherited_merge_info,
              avoid_repeated_merge_on_subtree_with_merge_info,
              obey_reporter_api_semantics_while_doing_subtree_merges,
              SkipUnless(mergeinfo_inheritance,
                         server_has_mergeinfo),
              mergeinfo_elision,
              mergeinfo_inheritance_and_discontinuous_ranges,
              SkipUnless(merge_to_target_with_copied_children,
                         server_has_mergeinfo),
              merge_to_switched_path,
              SkipUnless(merge_to_path_with_switched_children,
                         server_has_mergeinfo),
              merge_with_implicit_target_file,
              SkipUnless(empty_rev_range_mergeinfo,
                         server_has_mergeinfo),
              detect_copy_src_for_target_with_multiple_ancestors,
              prop_add_to_child_with_mergeinfo,
              diff_repos_does_not_update_mergeinfo,
              XFail(avoid_reflected_revs),
              Skip(mergeinfo_and_skipped_paths, svntest.main.is_ra_type_file),
              update_loses_mergeinfo,
              merge_loses_mergeinfo,
              single_file_replace_style_merge_capability,
              merge_to_out_of_date_target,
              merge_with_depth_files,
              merge_fails_if_subtree_is_deleted_on_src,
              no_mergeinfo_from_no_op_merge,
              merge_to_sparse_directories,
              merge_old_and_new_revs_from_renamed_dir,
              merge_with_child_having_different_rev_ranges_to_merge,
              merge_old_and_new_revs_from_renamed_file,
              merge_with_auto_rev_range_detection,
              XFail(mergeinfo_recording_in_skipped_merge),
              cherry_picking,
              propchange_of_subdir_raises_conflict,
              reverse_merge_prop_add_on_child,
              XFail(merge_target_with_non_inheritable_mergeinfo),
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

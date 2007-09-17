#!/usr/bin/env python
#
#  depth_tests.py:  Testing that operations work as expected at
#                   various depths (depth-empty, depth-files,
#                   depth-immediates, depth-infinity).
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2007 CollabNet.  All rights reserved.
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
from svntest import wc, SVNAnyOutput

# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = wc.StateItem

# For errors setting up the depthy working copies.
class DepthSetupError(Exception):
  def __init__ (self, args=None):
    self.args = args

def set_up_depthy_working_copies(sbox, empty=False, files=False,
                                 immediates=False, infinity=False):
  """Set up up to four working copies, at various depths.  At least
  one of depths EMPTY, FILES, IMMEDIATES, or INFINITY must be passed
  as True.  The corresponding working copy paths are returned in a
  four-element tuple in that order, with element value of None for
  working copies that were not created.  If all args are False, raise
  DepthSetupError."""

  if not (infinity or empty or files or immediates):
    raise DepthSetupError("At least one working copy depth must be passed.")

  wc = None
  if infinity:
    sbox.build()
    wc = sbox.wc_dir
  else:
    sbox.build(create_wc = False)
    if os.path.exists(sbox.wc_dir):
      svntest.main.safe_rmtree(sbox.wc_dir)

  wc_empty = None
  if empty:
    wc_empty = sbox.wc_dir + '-depth-empty'
    if os.path.exists(wc_empty):
      svntest.main.safe_rmtree(wc_empty)
    svntest.actions.run_and_verify_svn(
      "Unexpected error from co --depth=empty",
      SVNAnyOutput, [], "co", "--depth", "empty", sbox.repo_url, wc_empty)

  wc_files = None
  if files:
    wc_files = sbox.wc_dir + '-depth-files'
    if os.path.exists(wc1):
      svntest.main.safe_rmtree(wc1)
    svntest.actions.run_and_verify_svn(
      "Unexpected error from co --depth=files",
      SVNAnyOutput, [], "co", "--depth", "files", sbox.repo_url, wc_files)

  wc_immediates = None
  if immediates:
    wc_immediates = sbox.wc_dir + '-depth-immediates'
    if os.path.exists(wc_immediates):
      svntest.main.safe_rmtree(wc_immediates)
    svntest.actions.run_and_verify_svn(
      "Unexpected error from co --depth=immediates",
      SVNAnyOutput, [], "co", "--depth", "immediates",
      sbox.repo_url, wc_immediates)

  return wc_empty, wc_files, wc_immediates, wc


#----------------------------------------------------------------------
# Ensure that 'checkout --depth=empty' results in a depth-empty working copy.
def depth_empty_checkout(sbox):
  "depth-empty checkout"

  wc_empty, ign_a, ign_b, ign_c = set_up_depthy_working_copies(sbox, empty=True)

  if os.path.exists(os.path.join(wc_empty, "iota")):
    raise svntest.Failure("depth-empty checkout created file 'iota'")

  if os.path.exists(os.path.join(wc_empty, "A")):
    raise svntest.Failure("depth-empty checkout created subdir 'A'")

  svntest.actions.run_and_verify_svn(
    "Expected depth empty for top of WC, got some other depth",
    "Depth: empty", [], "info", wc_empty)


# Helper for two test functions.
def depth_files_same_as_nonrecursive(sbox, opt):
  """Run a depth-files or non-recursive checkout, depending on whether
  passed '-N' or '--depth=files' for OPT.  The two should get the same
  result, hence this helper containing the common code between the
  two tests."""

  # This duplicates some code from set_up_depthy_working_copies(), but
  # that's because it's abstracting out a different axis.

  sbox.build(create_wc = False)
  if os.path.exists(sbox.wc_dir):
    svntest.main.safe_rmtree(sbox.wc_dir)

  svntest.actions.run_and_verify_svn("Unexpected error during co %s" % opt,
                                     SVNAnyOutput, [], "co", opt,
                                     sbox.repo_url,
                                     sbox.wc_dir)

  # Should create a depth-files top directory, so both iota and A
  # should exist, and A should be empty and depth-empty.

  if not os.path.exists(os.path.join(sbox.wc_dir, "iota")):
    raise svntest.Failure("'checkout %s' failed to create file 'iota'" % opt)

  if os.path.exists(os.path.join(sbox.wc_dir, "A")):
    raise svntest.Failure("'checkout %s' unexpectedly created subdir 'A'" % opt)

  svntest.actions.run_and_verify_svn(
    "Expected depth files for top of WC, got some other depth",
    "Depth: files", [], "info", sbox.wc_dir)


def depth_files_checkout(sbox):
  "depth-files checkout"
  depth_files_same_as_nonrecursive(sbox, "--depth=files")


def nonrecursive_checkout(sbox):
  "non-recursive checkout equals depth-files"
  depth_files_same_as_nonrecursive(sbox, "-N")


#----------------------------------------------------------------------
def depth_empty_update_bypass_single_file(sbox):
  "update depth-empty wc shouldn't receive file mod"

  wc_empty, ign_a, ign_b, wc = set_up_depthy_working_copies(sbox, empty=True,
                                                            infinity=True)

  iota_path = os.path.join(wc, 'iota')
  svntest.main.file_append(iota_path, "new text\n")

  # Commit in the "other" wc.
  expected_output = svntest.wc.State(wc, { 'iota' : Item(verb='Sending'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.tweak('iota', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-empty wc, expecting not to receive the change to iota.
  expected_output = svntest.wc.State(wc_empty, { })
  expected_disk = svntest.wc.State('', { })
  expected_status = svntest.wc.State(wc_empty, { '' : svntest.wc.StateItem() })
  expected_status.tweak(contents=None, status='  ', wc_rev=2)
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)


#----------------------------------------------------------------------
def depth_immediates_get_top_file_mod_only(sbox):
  "update depth-immediates wc gets top file mod only"

  ign_a, ign_b, wc_immediates, wc \
         = set_up_depthy_working_copies(sbox, immediates=True, infinity=True)

  iota_path = os.path.join(wc, 'iota')
  svntest.main.file_append(iota_path, "new text in iota\n")
  mu_path = os.path.join(wc, 'A', 'mu')
  svntest.main.file_append(mu_path, "new text in mu\n")

  # Commit in the "other" wc.
  expected_output = svntest.wc.State(wc,
                                     { 'iota' : Item(verb='Sending'),
                                       'A/mu' : Item(verb='Sending'),
                                       })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.tweak('iota', wc_rev=2, status='  ')
  expected_status.tweak('A/mu', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-immediates wc, expecting to receive only the
  # change to iota.
  expected_output = svntest.wc.State(wc_immediates,
                                     { 'iota' : Item(status='U ') })
  expected_disk = svntest.wc.State('', { })
  expected_disk.add(\
    {'iota' : Item(contents="This is the file 'iota'.\nnew text in iota\n"),
     'A' : Item(contents=None) } )
  expected_status = svntest.wc.State(wc_immediates,
                                     { '' : svntest.wc.StateItem() })
  expected_status.tweak(contents=None, status='  ', wc_rev=2)
  expected_status.add(\
    {'iota' : Item(status='  ', wc_rev=2),
     'A' : Item(status='  ', wc_rev=2) } )
  svntest.actions.run_and_verify_update(wc_immediates,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)


#----------------------------------------------------------------------
def depth_empty_commit(sbox):
  "commit a file from a depth-empty working copy"
  # Bring iota into a depth-empty working copy, then commit a change to it.
  raise svntest.Failure("<test not yet written>")

#----------------------------------------------------------------------
def depth_empty_with_file(sbox):
  "act on a file in a depth-empty working copy"
  # Run 'svn up iota' to bring iota permanently into the working copy.
  wc_empty, ign_a, ign_b, wc = set_up_depthy_working_copies(sbox, empty=True,
                                                            infinity=True)

  iota_path = os.path.join(wc_empty, 'iota')
  if os.path.exists(iota_path):
    raise svntest.Failure("'%s' exists when it shouldn't" % iota_path)

  # ### I'd love to do this using the recommended {expected_output,
  # ### expected_status, expected_disk} method here, but after twenty
  # ### minutes of trying to figure out how, I decided to compromise.

  # Update iota by name, expecting to receive it.
  svntest.actions.run_and_verify_svn(None, None, [], 'up', iota_path)

  # Test that we did receive it.
  if not os.path.exists(iota_path):
    raise svntest.Failure("'%s' doesn't exist when it should" % iota_path)

  # Commit a change to iota in the "other" wc.
  other_iota_path = os.path.join(wc, 'iota')
  svntest.main.file_append(other_iota_path, "new text\n")
  expected_output = svntest.wc.State(wc, { 'iota' : Item(verb='Sending'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.tweak('iota', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Delete iota in the "other" wc.
  other_iota_path = os.path.join(wc, 'iota')
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', other_iota_path)
  expected_output = svntest.wc.State(wc, { 'iota' : Item(verb='Deleting'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.remove('iota')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-empty wc, expecting to receive the deletion of iota.
  expected_output = svntest.wc.State(\
    wc_empty, { 'iota' : svntest.wc.StateItem(status='D ') })
  expected_disk = svntest.wc.State('', { })
  expected_status = svntest.wc.State(\
    wc_empty, { '' : svntest.wc.StateItem(status='  ', wc_rev=3) })
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)


#----------------------------------------------------------------------
def depth_empty_with_dir(sbox):
  "bring a dir into a depth-empty working copy"
  # Run 'svn up A' to bring A permanently into the working copy.
  wc_empty, ign_a, ign_b, wc = set_up_depthy_working_copies(sbox, empty=True,
                                                            infinity=True)

  A_path = os.path.join(wc_empty, 'A')
  other_mu_path = os.path.join(wc, 'A', 'mu')

  # We expect A to be added at depth infinity, so a normal 'svn up A'
  # should be sufficient to add all descendants.
  expected_output = svntest.wc.State(wc_empty, {
    'A'              : Item(status='A '),
    'A/mu'           : Item(status='A '),
    'A/B'            : Item(status='A '),
    'A/B/lambda'     : Item(status='A '),
    'A/B/E'          : Item(status='A '),
    'A/B/E/alpha'    : Item(status='A '),
    'A/B/E/beta'     : Item(status='A '),
    'A/B/F'          : Item(status='A '),
    'A/C'            : Item(status='A '),
    'A/D'            : Item(status='A '),
    'A/D/gamma'      : Item(status='A '),
    'A/D/G'          : Item(status='A '),
    'A/D/G/pi'       : Item(status='A '),
    'A/D/G/rho'      : Item(status='A '),
    'A/D/G/tau'      : Item(status='A '),
    'A/D/H'          : Item(status='A '),
    'A/D/H/chi'      : Item(status='A '),
    'A/D/H/psi'      : Item(status='A '),
    'A/D/H/omega'    : Item(status='A ')
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('iota')
  expected_status = svntest.actions.get_virginal_state(wc_empty, 1)
  expected_status.remove('iota')
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        A_path)

  # Commit a change to A/mu in the "other" wc.
  svntest.main.file_write(other_mu_path, "new text\n")
  expected_output = svntest.wc.State(\
    wc, { 'A/mu' : Item(verb='Sending'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.tweak('A/mu', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update "A" by name in wc_empty, expect to receive the change to A/mu.
  expected_output = svntest.wc.State(wc_empty, { 'A/mu' : Item(status='U ') })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('iota')
  expected_disk.tweak('A/mu', contents='new text\n')
  expected_status = svntest.actions.get_virginal_state(wc_empty, 2)
  expected_status.remove('iota')
  expected_status.tweak('', wc_rev=1)
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        A_path)

  # Commit the deletion of A/mu from the "other" wc.
  svntest.main.file_write(other_mu_path, "new text\n")
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', other_mu_path)
  expected_output = svntest.wc.State(wc, { 'A/mu' : Item(verb='Deleting'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.remove('A/mu')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)


  # Update "A" by name in wc_empty, expect to A/mu to disappear.
  expected_output = svntest.wc.State(wc_empty, { 'A/mu' : Item(status='D ') })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('iota')
  expected_disk.remove('A/mu')
  expected_status = svntest.actions.get_virginal_state(wc_empty, 3)
  expected_status.remove('iota')
  expected_status.remove('A/mu')
  expected_status.tweak('', wc_rev=1)
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        A_path)



#----------------------------------------------------------------------
def depth_immediates_bring_in_file(sbox):
  "bring a file into a depth-immediates working copy"
  # Run 'svn up A/mu' to bring A/mu permanently into the working copy.
  # How should 'svn up A/D/gamma' behave, however?  Edge cases...
  raise svntest.Failure("<test not yet written>")

#----------------------------------------------------------------------
def depth_immediates_fill_in_dir(sbox):
  "bring a dir into a depth-immediates working copy"

  # Run 'svn up A' to fill in A as a depth-infinity subdir.
  ign_a, ign_b, wc_immediates, wc \
                        = set_up_depthy_working_copies(sbox, immediates=True)
  A_path = os.path.join(wc_immediates, 'A')
  expected_output = svntest.wc.State(wc_immediates, {
    'A/mu'           : Item(status='A '),
    'A/B'            : Item(status='A '),
    'A/B/lambda'     : Item(status='A '),
    'A/B/E'          : Item(status='A '),
    'A/B/E/alpha'    : Item(status='A '),
    'A/B/E/beta'     : Item(status='A '),
    'A/B/F'          : Item(status='A '),
    'A/C'            : Item(status='A '),
    'A/D'            : Item(status='A '),
    'A/D/gamma'      : Item(status='A '),
    'A/D/G'          : Item(status='A '),
    'A/D/G/pi'       : Item(status='A '),
    'A/D/G/rho'      : Item(status='A '),
    'A/D/G/tau'      : Item(status='A '),
    'A/D/H'          : Item(status='A '),
    'A/D/H/chi'      : Item(status='A '),
    'A/D/H/psi'      : Item(status='A '),
    'A/D/H/omega'    : Item(status='A ')
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_status = svntest.actions.get_virginal_state(wc_immediates, 1)
  svntest.actions.run_and_verify_update(wc_immediates,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'infinity',
                                        A_path)

#----------------------------------------------------------------------
def depth_mixed_bring_in_dir(sbox):
  "bring a dir into a mixed-depth working copy"

  # Run 'svn up --depth=immediates A' in a depth-empty working copy.
  wc_empty, ign_a, ign_b, wc = set_up_depthy_working_copies(sbox, empty=True)
  A_path = os.path.join(wc_empty, 'A')
  B_path = os.path.join(wc_empty, 'A', 'B')
  C_path = os.path.join(wc_empty, 'A', 'C')

  expected_output = svntest.wc.State(wc_empty, {
    'A'              : Item(status='A '),
    'A/mu'           : Item(status='A '),
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('iota', 'A/B', 'A/B/lambda', 'A/B/E', 'A/B/E/alpha',
                       'A/B/E/beta', 'A/B/F', 'A/C', 'A/D', 'A/D/gamma',
                       'A/D/G', 'A/D/G/pi', 'A/D/G/rho', 'A/D/G/tau',
                       'A/D/H', 'A/D/H/chi', 'A/D/H/psi', 'A/D/H/omega')
  expected_status = svntest.actions.get_virginal_state(wc_empty, 1)
  expected_status.remove('iota', 'A/B', 'A/B/lambda', 'A/B/E', 'A/B/E/alpha',
                         'A/B/E/beta', 'A/B/F', 'A/C', 'A/D', 'A/D/gamma',
                         'A/D/G', 'A/D/G/pi', 'A/D/G/rho', 'A/D/G/tau',
                         'A/D/H', 'A/D/H/chi', 'A/D/H/psi', 'A/D/H/omega')
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'files',
                                        A_path)
  # Check that A was added at depth=files.
  svntest.actions.run_and_verify_svn(None, "Depth: files", [], "info",
                                     A_path)

  # Now, bring in A/B at depth-immediates.
  expected_output = svntest.wc.State(wc_empty, {
    'A/B'            : Item(status='A '),
    'A/B/lambda'     : Item(status='A '),
    'A/B/E'          : Item(status='A '),
    'A/B/F'          : Item(status='A '),
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('iota', 'A/B/E/alpha', 'A/B/E/beta', 'A/C',
                       'A/D', 'A/D/gamma', 'A/D/G', 'A/D/G/pi', 'A/D/G/rho',
                       'A/D/G/tau', 'A/D/H', 'A/D/H/chi', 'A/D/H/psi',
                       'A/D/H/omega')
  expected_status = svntest.actions.get_virginal_state(wc_empty, 1)
  expected_status.remove('iota', 'A/B/E/alpha', 'A/B/E/beta', 'A/C',
                         'A/D', 'A/D/gamma', 'A/D/G', 'A/D/G/pi', 'A/D/G/rho',
                         'A/D/G/tau', 'A/D/H', 'A/D/H/chi', 'A/D/H/psi',
                         'A/D/H/omega')
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'immediates',
                                        B_path)
  # Check that A/B was added at depth=immediates.
  svntest.actions.run_and_verify_svn(None, "Depth: immediates", [], "info",
                                     B_path)

  # Now, bring in A/C at depth-empty.
  expected_output = svntest.wc.State(wc_empty, {
    'A/C'            : Item(status='A '),
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('iota', 'A/B/E/alpha', 'A/B/E/beta',
                       'A/D', 'A/D/gamma', 'A/D/G', 'A/D/G/pi', 'A/D/G/rho',
                       'A/D/G/tau', 'A/D/H', 'A/D/H/chi', 'A/D/H/psi',
                       'A/D/H/omega')
  expected_status = svntest.actions.get_virginal_state(wc_empty, 1)
  expected_status.remove('iota', 'A/B/E/alpha', 'A/B/E/beta',
                         'A/D', 'A/D/gamma', 'A/D/G', 'A/D/G/pi', 'A/D/G/rho',
                         'A/D/G/tau', 'A/D/H', 'A/D/H/chi', 'A/D/H/psi',
                         'A/D/H/omega')
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'empty',
                                        C_path)
  # Check that A/C was added at depth=empty.
  svntest.actions.run_and_verify_svn(None, "Depth: empty", [], "info",
                                     C_path)

#----------------------------------------------------------------------
def depth_empty_unreceive_delete(sbox):
  "depth-empty working copy ignores a deletion"
  # Check out a depth-empty greek tree to wc1.  In wc2, delete iota and
  # commit.  Update wc1; should not receive the delete.
  wc_empty, ign_a, ign_b, wc = set_up_depthy_working_copies(sbox, empty=True,
                                                            infinity=True)

  iota_path = os.path.join(wc, 'iota')

  # Commit in the "other" wc.
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', iota_path)
  expected_output = svntest.wc.State(wc, { 'iota' : Item(verb='Deleting'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.remove('iota')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-0 wc, expecting not to receive the deletion of iota.
  expected_output = svntest.wc.State(wc_empty, { })
  expected_disk = svntest.wc.State('', { })
  expected_status = svntest.wc.State(wc_empty, { '' : svntest.wc.StateItem() })
  expected_status.tweak(contents=None, status='  ', wc_rev=2)
  svntest.actions.run_and_verify_update(wc_empty,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)


#----------------------------------------------------------------------
def depth_immediates_unreceive_delete(sbox):
  "depth-immediates working copy ignores a deletion"
  # Check out a depth-immediates greek tree to wc1.  In wc2, delete
  # A/mu and commit.  Update wc1; should not receive the delete.

  ign_a, ign_b, wc_immed, wc = set_up_depthy_working_copies(sbox,
                                                            immediates=True,
                                                            infinity=True)

  mu_path = os.path.join(wc, 'A', 'mu')

  # Commit in the "other" wc.
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', mu_path)
  expected_output = svntest.wc.State(wc, { 'A/mu' : Item(verb='Deleting'), })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.remove('A/mu')
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-immediates wc, expecting not to receive the deletion
  # of A/mu.
  expected_output = svntest.wc.State(wc_immed, { })
  expected_disk = svntest.wc.State('', {
    'iota' : Item(contents="This is the file 'iota'.\n"),
    'A' : Item()
    })
  expected_status = svntest.wc.State(wc_immed, {
    '' : Item(status='  ', wc_rev=2),
    'iota' : Item(status='  ', wc_rev=2),
    'A' : Item(status='  ', wc_rev=2)
    })
  svntest.actions.run_and_verify_update(wc_immed,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)

#----------------------------------------------------------------------
def depth_immediates_receive_delete(sbox):
  "depth-1 working copy receives a deletion"
  # Check out a depth-immediates greek tree to wc1.  In wc2, delete A and
  # commit.  Update wc1  should receive the delete.

  ign_a, ign_b, wc_immed, wc = set_up_depthy_working_copies(sbox,
                                                            immediates=True,
                                                            infinity=True)

  A_path = os.path.join(wc, 'A')

  # Commit in the "other" wc.
  svntest.actions.run_and_verify_svn(None, None, [], 'rm', A_path)
  expected_output = svntest.wc.State(wc, { 'A' : Item(verb='Deleting'), })
  expected_status = svntest.wc.State(wc, {
    '' : Item(status='  ', wc_rev=1),
    'iota' : Item(status='  ', wc_rev=1),
    })
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-immediates wc, expecting to receive the deletion of A.
  expected_output = svntest.wc.State(wc_immed, {
    'A'    : Item(status='D ')
    })
  expected_disk = svntest.wc.State('', {
    'iota' : Item(contents="This is the file 'iota'.\n"),
    })
  expected_status = svntest.wc.State(wc_immed, {
    ''     : Item(status='  ', wc_rev=2),
    'iota' : Item(status='  ', wc_rev=2)
    })
  svntest.actions.run_and_verify_update(wc_immed,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)

#----------------------------------------------------------------------
def depth_immediates_subdir_propset_1(sbox):
  "depth-1 commit subdirectory propset, then update"
  ign_a, ign_b, wc_immediates, ign_c \
         = set_up_depthy_working_copies(sbox, immediates=True)

  A_path = os.path.join(wc_immediates, 'A')

  # Set a property on an immediate subdirectory of the working copy.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'pset', 'foo', 'bar',
                                     A_path)

  # Create expected output tree.
  expected_output = svntest.wc.State(wc_immediates, {
    'A' : Item(verb='Sending'),
    })

  # Create expected status tree.
  expected_status = svntest.wc.State(wc_immediates, {
    '' : Item(status='  ', wc_rev=1),
    'iota' : Item(status='  ', wc_rev=1),
    'A' : Item(status='  ', wc_rev=2)
    })

  # Commit wc_immediates/A.
  svntest.actions.run_and_verify_commit(wc_immediates,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        A_path)

  # Create expected output tree for the update.
  expected_output = svntest.wc.State(wc_immediates, { })

  # Create expected disk tree.
  expected_disk = svntest.wc.State('', {
    'iota' : Item(contents="This is the file 'iota'.\n"),
    'A' : Item(contents=None, props={'foo' : 'bar'}),
    })

  expected_status.tweak(contents=None, status='  ', wc_rev=2)

  # Update the depth-immediates wc.
  svntest.actions.run_and_verify_update(wc_immediates,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None, 1)

#----------------------------------------------------------------------
def depth_immediates_subdir_propset_2(sbox):
  "depth-1 update receives a subdirectory propset"
  sbox.build()
  wc_dir = sbox.wc_dir

  # Make the other working copy.
  other_wc = sbox.add_wc_path('other')
  svntest.actions.duplicate_dir(wc_dir, other_wc)

  A_path = os.path.join(wc_dir, 'A')

  # Set a property on an immediate subdirectory of the working copy.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'pset', 'foo', 'bar',
                                     A_path)
  # Commit.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'commit', '-m', 'logmsg', A_path)

  # Update at depth=immediates in the other wc, expecting to see no errors.
  svntest.actions.run_and_verify_svn("Output on stderr where none expected",
                                     SVNAnyOutput, [],
                                     'update', '--depth', 'immediates',
                                     other_wc)

#----------------------------------------------------------------------
def depth_update_to_more_depth(sbox):
  "gradually update an empty wc to depth=infinity"

  wc_dir, ign_a, ign_b, ign_c = set_up_depthy_working_copies(sbox, empty=True)

  os.chdir(wc_dir)

  # Run 'svn up --depth=files' in a depth-empty working copy.
  expected_output = svntest.wc.State('', {
    'iota'              : Item(status='A '),
    })
  expected_status = svntest.wc.State('', {
    '' : Item(status='  ', wc_rev=1),
    'iota' : Item(status='  ', wc_rev=1),
    })
  expected_disk = svntest.wc.State('', {
    'iota' : Item("This is the file 'iota'.\n"),
    })
  svntest.actions.run_and_verify_update('',
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'files')
  svntest.actions.run_and_verify_svn(None, "Depth: files", [], "info")

  # Run 'svn up --depth=immediates' in the now depth-files working copy.
  expected_output = svntest.wc.State('', {
    'A'              : Item(status='A '),
    })
  expected_status = svntest.wc.State('', {
    '' : Item(status='  ', wc_rev=1),
    'iota' : Item(status='  ', wc_rev=1),
    'A' : Item(status='  ', wc_rev=1),
    })
  expected_disk = svntest.wc.State('', {
    'iota' : Item("This is the file 'iota'.\n"),
    'A'    : Item(),
    })
  svntest.actions.run_and_verify_update('',
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'immediates')
  svntest.actions.run_and_verify_svn(None, "Depth: immediates", [], "info")
  svntest.actions.run_and_verify_svn(None, "Depth: empty", [], "info", "A")

  # Upgrade 'A' to depth-files.
  expected_output = svntest.wc.State('', {
    'A/mu'           : Item(status='A '),
    })
  expected_status = svntest.wc.State('', {
    '' : Item(status='  ', wc_rev=1),
    'iota' : Item(status='  ', wc_rev=1),
    'A' : Item(status='  ', wc_rev=1),
    'A/mu' : Item(status='  ', wc_rev=1),
    })
  expected_disk = svntest.wc.State('', {
    'iota' : Item("This is the file 'iota'.\n"),
    'A'    : Item(),
    'A/mu' : Item("This is the file 'mu'.\n"),
    })
  svntest.actions.run_and_verify_update('',
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'files', 'A')
  svntest.actions.run_and_verify_svn(None, "Depth: immediates", [], "info")
  svntest.actions.run_and_verify_svn(None, "Depth: files", [], "info", "A")

  # Run 'svn up --depth=infinity' in the working copy.
  expected_output = svntest.wc.State('', {
    'A/B'            : Item(status='A '),
    'A/B/lambda'     : Item(status='A '),
    'A/B/E'          : Item(status='A '),
    'A/B/E/alpha'    : Item(status='A '),
    'A/B/E/beta'     : Item(status='A '),
    'A/B/F'          : Item(status='A '),
    'A/C'            : Item(status='A '),
    'A/D'            : Item(status='A '),
    'A/D/gamma'      : Item(status='A '),
    'A/D/G'          : Item(status='A '),
    'A/D/G/pi'       : Item(status='A '),
    'A/D/G/rho'      : Item(status='A '),
    'A/D/G/tau'      : Item(status='A '),
    'A/D/H'          : Item(status='A '),
    'A/D/H/chi'      : Item(status='A '),
    'A/D/H/psi'      : Item(status='A '),
    'A/D/H/omega'    : Item(status='A ')
    })
  expected_disk = svntest.main.greek_state.copy()
  expected_status = svntest.actions.get_virginal_state('', 1)
  svntest.actions.run_and_verify_update('',
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None,
                                        None, None, None, None,
                                        '--depth', 'infinity')
  # svn info doesn't print a 'Depth:' line for infinity, so we verify
  # that no such line is present in the output.
  output1, err = svntest.actions.run_and_verify_svn(None, None, [], "info")
  output2, err = svntest.actions.run_and_verify_svn(None, None, [], "info", "A")
  for line in output1 + output2:
    if line.startswith("Depth:"):
      raise svntest.Failure("Non-infinity depth detected after an upgrade \
                             to depth-infinity")

def commit_propmods_with_depth_empty(sbox):
  "commit property mods only, using --depth=empty"
  sbox.build()
  wc_dir = sbox.wc_dir

  iota_path = os.path.join(wc_dir, 'iota')
  A_path = os.path.join(wc_dir, 'A')
  D_path = os.path.join(A_path, 'D')
  gamma_path = os.path.join(D_path, 'gamma')
  G_path = os.path.join(D_path, 'G')
  pi_path = os.path.join(G_path, 'pi')
  H_path = os.path.join(D_path, 'H')
  chi_path = os.path.join(H_path, 'chi')

  # Set some properties, modify some files.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo-val', wc_dir)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'bar', 'bar-val', D_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'baz', 'baz-val', G_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'qux', 'qux-val', H_path)
  svntest.main.file_append(iota_path, "new iota\n")
  svntest.main.file_append(gamma_path, "new gamma\n")
  svntest.main.file_append(pi_path, "new pi\n")
  svntest.main.file_append(chi_path, "new chi\n")

  # The only things that should be committed are two of the propsets.
  expected_output = svntest.wc.State(
    wc_dir,
    { ''    : Item(verb='Sending'),
      'A/D' : Item(verb='Sending'), }
    )
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  # Expect the two propsets to be committed:
  expected_status.tweak('', status='  ', wc_rev=2)
  expected_status.tweak('A/D', status='  ', wc_rev=2)
  # Expect every other change to remain uncommitted:
  expected_status.tweak('iota', status='M ', wc_rev=1)
  expected_status.tweak('A/D/G', status=' M', wc_rev=1)
  expected_status.tweak('A/D/H', status=' M', wc_rev=1)
  expected_status.tweak('A/D/gamma', status='M ', wc_rev=1)
  expected_status.tweak('A/D/G/pi', status='M ', wc_rev=1)
  expected_status.tweak('A/D/H/chi', status='M ', wc_rev=1)

  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        '--depth=empty',
                                        wc_dir, D_path)

# Test for issue #2845.
def diff_in_depthy_wc(sbox):
  "diff at various depths in non-infinity wc"

  wc_empty, ign_a, ign_b, wc = set_up_depthy_working_copies(sbox, empty=True,
                                                            infinity=True)

  iota_path = os.path.join(wc, 'iota')
  A_path = os.path.join(wc, 'A')
  mu_path = os.path.join(wc, 'A', 'mu')
  gamma_path = os.path.join(wc, 'A', 'D', 'gamma')

  # Make some changes in the depth-infinity wc, and commit them
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'foo', 'foo-val', wc)
  svntest.main.file_write(iota_path, "new text\n")
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'bar', 'bar-val', A_path)  
  svntest.main.file_write(mu_path, "new text\n")
  svntest.main.file_write(gamma_path, "new text\n")
  svntest.actions.run_and_verify_svn(None, None, [], 'commit', '-m', '', wc)

  diff = [
    "\n",
    "Property changes on: .\n",
    "___________________________________________________________________\n",
    "Name: foo\n",
    "   - foo-val\n",
    "\n",
    "Index: iota\n",
    "===================================================================\n",
    "--- iota\t(revision 2)\n",
    "+++ iota\t(working copy)\n",
    "@@ -1 +1 @@\n",
    "-new text\n",
    "+This is the file 'iota'.\n",
    "Property changes on: A\n",
    "___________________________________________________________________\n",
    "Name: bar\n",
    "   - bar-val\n",
    "\n",
    "\n",
    "Index: A/mu\n",
    "===================================================================\n",
    "--- A/mu\t(revision 2)\n",
    "+++ A/mu\t(working copy)\n",
    "@@ -1 +1 @@\n",
    "-new text\n",
    "+This is the file 'mu'.\n" ]

  os.chdir(wc_empty)

  expected_output = svntest.actions.UnorderedOutput(diff[:6])
  # The diff should contain only the propchange on '.'
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '-rHEAD')

  # Upgrade to depth-files.
  svntest.actions.run_and_verify_svn(None, None, [], 'up',
                                     '--depth', 'files', '-r1')
  # The diff should contain only the propchange on '.' and the
  # contents change on iota.
  expected_output = svntest.actions.UnorderedOutput(diff[:13])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '-rHEAD')
  # Do a diff at --depth empty.
  expected_output = svntest.actions.UnorderedOutput(diff[:6])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '--depth', 'empty', '-rHEAD')

  # Upgrade to depth-immediates.
  svntest.actions.run_and_verify_svn(None, None, [], 'up',
                                     '--depth', 'immediates', '-r1')
  # The diff should contain the propchanges on '.' and 'A' and the
  # contents change on iota.
  expected_output = svntest.actions.UnorderedOutput(diff[:19])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                    'diff', '-rHEAD')
  # Do a diff at --depth files.
  expected_output = svntest.actions.UnorderedOutput(diff[:13])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '--depth', 'files', '-rHEAD')

  # Upgrade A to depth-files.
  svntest.actions.run_and_verify_svn(None, None, [], 'up',
                                     '--depth', 'files', '-r1', 'A')
  # The diff should contain everything but the contents change on
  # gamma (which does not exist in this working copy).
  expected_output = svntest.actions.UnorderedOutput(diff)
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '-rHEAD')
  # Do a diff at --depth immediates.
  expected_output = svntest.actions.UnorderedOutput(diff[:19])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                    'diff', '--depth', 'immediates', '-rHEAD')

def commit_depth_immediates(sbox):
  "commit some files with --depth=immediates"
  sbox.build()
  wc_dir = sbox.wc_dir

  # Test the fix for some bugs Mike Pilato reported here:
  #
  #    http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgNo=128509
  #    From: "C. Michael Pilato" <cmpilato@collab.net>
  #    To: Karl Fogel <kfogel@red-bean.com>
  #    CC: dev@subversion.tigris.org
  #    References: <87d4yzcrro.fsf@red-bean.com>
  #    Subject: Re: [PATCH] Make 'svn commit --depth=foo' work.
  #    Message-ID: <46968831.2070906@collab.net>
  #    Date: Thu, 12 Jul 2007 15:59:45 -0400
  #
  # See also http://subversion.tigris.org/issues/show_bug.cgi?id=2882.
  #
  # Outline of the test:
  # ====================
  #
  # Modify these three files:
  #
  #    M      A/mu
  #    M      A/D/G/rho
  #    M      iota
  # 
  # Then commit some of them using --depth=immediates:
  #
  #    svn ci -m "log msg" --depth=immediates wc_dir wc_dir/A/D/G/rho
  #
  # Before the bugfix, that would result in an error:
  #
  #    subversion/libsvn_wc/lock.c:570: (apr_err=155004)
  #    svn: Working copy '/blah/blah/blah/wc' locked
  #    svn: run 'svn cleanup' to remove locks \
  #         (type 'svn help cleanup' for details)
  #
  # After the bugfix, it correctly commits two of the three files:
  #
  #    Sending        A/D/G/rho
  #    Sending        iota
  #    Transmitting file data ..
  #    Committed revision 2.

  iota_path = os.path.join(wc_dir, 'iota')
  mu_path   = os.path.join(wc_dir, 'A', 'mu')
  G_path    = os.path.join(wc_dir, 'A', 'D', 'G')
  rho_path  = os.path.join(G_path, 'rho')

  svntest.main.file_append(iota_path, "new text in iota\n")
  svntest.main.file_append(mu_path,   "new text in mu\n")
  svntest.main.file_append(rho_path,  "new text in rho\n")

  expected_output = svntest.wc.State(wc_dir, {
    'iota' : Item(verb='Sending'),
    'A/D/G/rho' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('iota',       status='  ',  wc_rev=2)
  expected_status.tweak('A/mu',       status='M ',  wc_rev=1)
  expected_status.tweak('A/D/G/rho',  status='  ',  wc_rev=2)
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        None, None,
                                        None, None,
                                        '--depth', 'immediates',
                                        wc_dir, G_path)

def depth_immediates_receive_new_dir(sbox):
  "depth-1 working copy receives a new directory"

  ign_a, ign_b, wc_immed, wc = set_up_depthy_working_copies(sbox,
                                                            immediates=True,
                                                            infinity=True)

  I_path = os.path.join(wc, 'I')
  zeta_path = os.path.join(wc, 'I', 'zeta')
  other_I_path = os.path.join(wc_immed, 'I')

  os.mkdir(I_path)
  svntest.main.file_write(zeta_path, "This is the file 'zeta'.\n")

  # Commit in the "other" wc.
  svntest.actions.run_and_verify_svn(None, None, [], 'add', I_path)
  expected_output = svntest.wc.State(wc, {
    'I'      : Item(verb='Adding'),
    'I/zeta' : Item(verb='Adding'),
    })
  expected_status = svntest.actions.get_virginal_state(wc, 1)
  expected_status.add({
    'I'      : Item(status='  ', wc_rev=2),
    'I/zeta' : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_commit(wc,
                                        expected_output,
                                        expected_status,
                                        None, None, None, None, None, wc)

  # Update the depth-immediates wc, expecting to receive just the
  # new directory, without the file.
  expected_output = svntest.wc.State(wc_immed, {
    'I'    : Item(status='A '),
    })
  expected_disk = svntest.wc.State('', {
    'iota' : Item(contents="This is the file 'iota'.\n"),
    'A'    : Item(),
    'I'    : Item(),
    })
  expected_status = svntest.wc.State(wc_immed, {
    ''     : Item(status='  ', wc_rev=2),
    'iota' : Item(status='  ', wc_rev=2),
    'A'    : Item(status='  ', wc_rev=2),
    'I'    : Item(status='  ', wc_rev=2),
    })
  svntest.actions.run_and_verify_update(wc_immed,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None, None, None)
  # Check that the new directory was added at depth=empty.
  svntest.actions.run_and_verify_svn(None, "Depth: empty", [], "info",
                                     other_I_path)

def add_tree_with_depth_files(sbox):
  "add multi-subdir tree with --depth=files"  # For issue #2931
  sbox.build()
  wc_dir = sbox.wc_dir
  new1_path = os.path.join(wc_dir, 'new1')
  new2_path = os.path.join(new1_path, 'new2')
  os.mkdir(new1_path)
  os.mkdir(new2_path)
  svntest.actions.run_and_verify_svn(None, None, [],
                                     "add", "--depth", "files", new1_path)

#----------------------------------------------------------------------

# list all tests here, starting with None:
test_list = [ None,
              depth_empty_checkout,
              depth_files_checkout,
              nonrecursive_checkout,
              depth_empty_update_bypass_single_file,
              depth_immediates_get_top_file_mod_only,
              XFail(depth_empty_commit),
              depth_empty_with_file,
              depth_empty_with_dir,
              XFail(depth_immediates_bring_in_file),
              depth_immediates_fill_in_dir,
              depth_mixed_bring_in_dir,
              depth_empty_unreceive_delete,
              depth_immediates_unreceive_delete,
              depth_immediates_receive_delete,
              depth_update_to_more_depth,
              depth_immediates_subdir_propset_1,
              depth_immediates_subdir_propset_2,
              commit_propmods_with_depth_empty,
              diff_in_depthy_wc,
              commit_depth_immediates,
              depth_immediates_receive_new_dir,
              add_tree_with_depth_files,
            ]

if __name__ == "__main__":
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

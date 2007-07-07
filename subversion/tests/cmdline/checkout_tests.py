#!/usr/bin/env python
#
#  checkout_tests.py:  Testing checkout --force behavior when local
#                      tree already exits.
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
import sys, re, os, time

# Our testing module
import svntest
from svntest import wc, SVNAnyOutput

# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = wc.StateItem

#----------------------------------------------------------------------
# Helper function for testing stderr from co.
# If none of the strings in STDERR list matches the regular expression
# RE_STRING raise an error.
def test_stderr(re_string, stderr):
  exp_err_re = re.compile(re_string)
  for line in stderr:
    if exp_err_re.search(line):
      return
  raise svntest.Failure("Checkout failed but not in the expected way")

#----------------------------------------------------------------------
# Helper function to set up an existing local tree that has paths which
# obstruct with the incoming WC.
#
# Build a sandbox SBOX without a WC.  Created the following paths
# rooted at SBOX.WC_DIR:
#
#    iota
#    A/
#    A/mu
#
# If MOD_FILES is FALSE, 'iota' and 'A/mu' have the same contents as the
# standard greek tree.  If TRUE the contents of each as set as follows:
#
#    iota : contents == "This is the local version of the file 'iota'.\n"
#    A/mu : contents == "This is the local version of the file 'mu'.\n"
#
# If ADD_UNVERSIONED is TRUE, add the following files and directories,
# rooted in SBOX.WC_DIR, that don't exist in the standard greek tree:
#
#    'sigma'
#    'A/upsilon'
#    'A/Z/'
#
# Return the expected output for svn co --force SBOX.REPO_URL SBOX.WC_DIR
#
def make_local_tree(sbox, mod_files=False, add_unversioned=False):
  """Make a local unversioned tree to checkout into."""

  sbox.build(create_wc = False)

  if os.path.exists(sbox.wc_dir):
    svntest.main.safe_rmtree(sbox.wc_dir)

  export_target = sbox.wc_dir
  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = sbox.wc_dir
  expected_output.desc[""] = Item()
  expected_output.tweak(contents=None, status="A ")

  # Export an unversioned tree to sbox.wc_dir.
  svntest.actions.run_and_verify_export(sbox.repo_url,
                                        export_target,
                                        expected_output,
                                        svntest.main.greek_state.copy())

  # Remove everything remaining except for 'iota', 'A/', and 'A/mu'.
  svntest.main.safe_rmtree(os.path.join(sbox.wc_dir, "A", "B"))
  svntest.main.safe_rmtree(os.path.join(sbox.wc_dir, "A", "C"))
  svntest.main.safe_rmtree(os.path.join(sbox.wc_dir, "A", "D"))

  # Should obstructions differ from the standard greek tree?
  if mod_files:
    iota_path = os.path.join(sbox.wc_dir, "iota")
    mu_path = os.path.join(sbox.wc_dir, "A", "mu")
    svntest.main.file_write(iota_path,
                            "This is the local version of the file 'iota'.\n")
    svntest.main.file_write(mu_path,
                            "This is the local version of the file 'mu'.\n")

  # Add some files that won't obstruct anything in standard greek tree?
  if add_unversioned:
    sigma_path = os.path.join(sbox.wc_dir, "sigma")
    svntest.main.file_append(sigma_path, "unversioned sigma")
    upsilon_path = os.path.join(sbox.wc_dir, "A", "upsilon")
    svntest.main.file_append(upsilon_path, "unversioned upsilon")
    Z_path = os.path.join(sbox.wc_dir, "A", "Z")
    os.mkdir(Z_path)

  return wc.State(sbox.wc_dir, {
    "A"           : Item(status='E '), # Obstruction
    "A/B"         : Item(status='A '),
    "A/B/lambda"  : Item(status='A '),
    "A/B/E"       : Item(status='A '),
    "A/B/E/alpha" : Item(status='A '),
    "A/B/E/beta"  : Item(status='A '),
    "A/B/F"       : Item(status='A '),
    "A/mu"        : Item(status='E '), # Obstruction
    "A/C"         : Item(status='A '),
    "A/D"         : Item(status='A '),
    "A/D/gamma"   : Item(status='A '),
    "A/D/G"       : Item(status='A '),
    "A/D/G/pi"    : Item(status='A '),
    "A/D/G/rho"   : Item(status='A '),
    "A/D/G/tau"   : Item(status='A '),
    "A/D/H"       : Item(status='A '),
    "A/D/H/chi"   : Item(status='A '),
    "A/D/H/omega" : Item(status='A '),
    "A/D/H/psi"   : Item(status='A '),
    "iota"        : Item(status='E '), # Obstruction
    })

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.
#----------------------------------------------------------------------

def checkout_with_obstructions(sbox):
  """co with obstructions should fail without --force"""

  make_local_tree(sbox, False, False)

  svntest.actions.run_and_verify_svn("No error where some expected",
                                     None, SVNAnyOutput, "co",
                                     sbox.repo_url,
                                     sbox.wc_dir)

#----------------------------------------------------------------------

def forced_checkout_of_file_with_dir_obstructions(sbox):
  """forced co fails if a dir obstructs a file"""

  make_local_tree(sbox, False, False)

  # Make the "other" working copy
  other_wc = sbox.add_wc_path('other')
  os.mkdir(other_wc)
  os.mkdir(os.path.join(other_wc, "iota"))

  # Checkout the standard greek repos into a directory that has a dir named
  # "iota" obstructing the file "iota" in the repos.  This should fail.
  sout, serr = svntest.actions.run_and_verify_svn("Expected error during co",
                                                  None, SVNAnyOutput, "co",
                                                  "--force", sbox.repo_url,
                                                  other_wc)

  test_stderr(".*Failed to add file.*a non-file object of the same name " \
              "already exists", serr)

#----------------------------------------------------------------------

def forced_checkout_of_dir_with_file_obstructions(sbox):
  """forced co fails if a file obstructs a dir"""

  make_local_tree(sbox, False, False)

  # Make the "other" working copy
  other_wc = sbox.add_wc_path('other')
  os.mkdir(other_wc)
  svntest.main.file_append(os.path.join(other_wc, "A"), "The file A")

  # Checkout the standard greek repos into a directory that has a file named
  # "A" obstructing the dir "A" in the repos.  This should fail.
  sout, serr = svntest.actions.run_and_verify_svn("Expected error during co",
                                                  None, SVNAnyOutput, "co",
                                                  "--force", sbox.repo_url,
                                                  other_wc)

  test_stderr(".*Failed to add directory.*a non-directory object of the " \
              "same name already exists", serr)

#----------------------------------------------------------------------

def forced_checkout_with_faux_obstructions(sbox):
  """co with faux obstructions ok with --force"""

  # Make a local tree that partially obstructs the paths coming from the
  # repos but has no true differences.
  expected_output = make_local_tree(sbox, False, False)

  expected_wc = svntest.main.greek_state.copy()

  svntest.actions.run_and_verify_checkout(sbox.repo_url,
                                          sbox.wc_dir, expected_output,
                                          expected_wc, None, None, None,
                                          None, '--force')

#----------------------------------------------------------------------

def forced_checkout_with_real_obstructions(sbox):
  """co with real obstructions ok with --force"""

  # Make a local tree that partially obstructs the paths coming from the
  # repos and make the obstructing files different from the standard greek
  # tree.
  expected_output = make_local_tree(sbox, True, False)

  expected_wc = svntest.main.greek_state.copy()
  expected_wc.tweak('A/mu',
                    contents="This is the local version of the file 'mu'.\n")
  expected_wc.tweak('iota',
                    contents="This is the local version of the file 'iota'.\n")

  svntest.actions.run_and_verify_checkout(sbox.repo_url,
                                          sbox.wc_dir, expected_output,
                                          expected_wc, None, None, None,
                                          None, '--force')

#----------------------------------------------------------------------

def forced_checkout_with_real_obstructions_and_unversioned_files(sbox):
  """co with real obstructions and unversioned files"""

  # Make a local tree that partially obstructs the paths coming from the
  # repos, make the obstructing files different from the standard greek
  # tree, and finally add some files that don't exist in the stardard tree.
  expected_output = make_local_tree(sbox, True, True)

  expected_wc = svntest.main.greek_state.copy()
  expected_wc.tweak('A/mu',
                    contents="This is the local version of the file 'mu'.\n")
  expected_wc.tweak('iota',
                    contents="This is the local version of the file 'iota'.\n")
  expected_wc.add({'sigma'     : Item("unversioned sigma"),
                   'A/upsilon' : Item("unversioned upsilon"),
                   'A/Z'       : Item(),
                   })

  svntest.actions.run_and_verify_checkout(sbox.repo_url,
                                          sbox.wc_dir, expected_output,
                                          expected_wc, None, None, None,
                                          None, '--force')

#----------------------------------------------------------------------

def forced_checkout_with_versioned_obstruction(sbox):
  """forced co with versioned obstruction"""

  # Make a greek tree working copy 
  sbox.build()
  
  # Create a second repository with the same greek tree
  repo_dir = sbox.repo_dir
  repo_url = sbox.repo_url
  other_repo_dir, other_repo_url = sbox.add_repo_path("other")
  svntest.main.copy_repos(repo_dir, other_repo_dir, 1, 1)

  other_wc_dir = sbox.add_wc_path("other")
  os.mkdir(other_wc_dir)

  # Checkout "A/" from the other repos.
  svntest.actions.run_and_verify_svn("Unexpected error during co",
                                     SVNAnyOutput, [], "co",
                                     other_repo_url + "/A",
                                     os.path.join(other_wc_dir, "A"))

  # Checkout the first repos into "other/A".  This should fail since the
  # obstructing versioned directory points to a different URL.
  sout, serr = svntest.actions.run_and_verify_svn("Expected error during co",
                                                  None, SVNAnyOutput, "co",
                                                  "--force", sbox.repo_url,
                                                  other_wc_dir)

  test_stderr("svn: Failed to add directory '.*A': a versioned directory " \
              "of the same name already exists", serr)

#----------------------------------------------------------------------
# Ensure that an import followed by a checkout in place works correctly.
def import_and_checkout(sbox):
  """import and checkout"""

  sbox.build()

  other_repo_dir, other_repo_url = sbox.add_repo_path("other")
  import_from_dir = sbox.add_wc_path("other")

  # Export greek tree to import_from_dir
  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = import_from_dir
  expected_output.desc[''] = Item()
  expected_output.tweak(contents=None, status='A ')
  svntest.actions.run_and_verify_export(sbox.repo_url,
                                        import_from_dir,
                                        expected_output,
                                        svntest.main.greek_state.copy())

  # Create the 'other' repos
  svntest.main.create_repos(other_repo_dir)

  # Import import_from_dir to the other repos
  expected_output = svntest.wc.State(sbox.wc_dir, {})

  svntest.actions.run_and_verify_svn(None, None, [], 'import',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '-m', 'import', import_from_dir,
                                     other_repo_url)

  expected_output = wc.State(import_from_dir, {
    "A"           : Item(status='E '),
    "A/B"         : Item(status='E '),
    "A/B/lambda"  : Item(status='E '),
    "A/B/E"       : Item(status='E '),
    "A/B/E/alpha" : Item(status='E '),
    "A/B/E/beta"  : Item(status='E '),
    "A/B/F"       : Item(status='E '),
    "A/mu"        : Item(status='E '),
    "A/C"         : Item(status='E '),
    "A/D"         : Item(status='E '),
    "A/D/gamma"   : Item(status='E '),
    "A/D/G"       : Item(status='E '),
    "A/D/G/pi"    : Item(status='E '),
    "A/D/G/rho"   : Item(status='E '),
    "A/D/G/tau"   : Item(status='E '),
    "A/D/H"       : Item(status='E '),
    "A/D/H/chi"   : Item(status='E '),
    "A/D/H/omega" : Item(status='E '),
    "A/D/H/psi"   : Item(status='E '),
    "iota"        : Item(status='E ')
    })

  expected_wc = svntest.main.greek_state.copy()

  svntest.actions.run_and_verify_checkout(other_repo_url, import_from_dir,
                                          expected_output, expected_wc,
                                          None, None, None, None,
                                          '--force')

#----------------------------------------------------------------------
# Issue #2529.
def checkout_broken_eol(sbox):
  "checkout file with broken eol style"

  svntest.actions.load_repo(sbox, os.path.join(os.path.dirname(sys.argv[0]),
                                               'update_tests_data',
                                               'checkout_broken_eol.dump'))

  URL = sbox.repo_url

  expected_output = svntest.wc.State(sbox.wc_dir, {
    'file': Item(status='A '),
    })
                                     
  expected_wc = svntest.wc.State('', {
    'file': Item(contents='line\nline2\n'),
    })
  svntest.actions.run_and_verify_checkout(URL,
                                          sbox.wc_dir,
                                          expected_output,
                                          expected_wc)

def checkout_creates_intermediate_folders(sbox):
  "checkout and create some intermediate folders"

  sbox.build(create_wc = False)

  checkout_target = os.path.join(sbox.wc_dir, 'a', 'b', 'c')
  
  # checkout a working copy in a/b/c, should create these intermediate 
  # folders
  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = checkout_target
  expected_output.tweak(status='A ', contents=None)

  expected_wc = svntest.main.greek_state
  
  svntest.actions.run_and_verify_checkout(sbox.repo_url,
                                          checkout_target,
                                          expected_output,
                                          expected_wc)

# Test that, if a peg revision is provided without an explicit revision, 
# svn will checkout the directory as it was at rPEG, rather than at HEAD.
def checkout_peg_rev(sbox):
  "checkout with peg revision"

  sbox.build()
  wc_dir = sbox.wc_dir
  # create a new revision
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  svntest.main.file_append(mu_path, 'appended mu text')

  svntest.actions.run_and_verify_svn(None, None, [],
                                    'ci', '-m', 'changed file mu', wc_dir)

  # now checkout the repo@1 in another folder, this should create our initial
  # wc without the change in mu.
  checkout_target = sbox.add_wc_path('checkout')
  os.mkdir(checkout_target)

  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = checkout_target
  expected_output.tweak(status='A ', contents=None)
  
  expected_wc = svntest.main.greek_state.copy()
  
  svntest.actions.run_and_verify_checkout(sbox.repo_url + '@1',
                                          checkout_target, 
                                          expected_output,
                                          expected_wc)

#----------------------------------------------------------------------
# Issue 2602: Test that peg revision dates are correctly supported. 
def checkout_peg_rev_date(sbox):
  "checkout with peg revision date"

  sbox.build()
  wc_dir = sbox.wc_dir

  # note the current time to use it as peg revision date.
  current_time = time.strftime("%Y-%m-%dT%H:%M:%S")
  
  # sleep till the next minute.
  current_sec = time.localtime().tm_sec
  time.sleep(62-current_sec)

  # create a new revision
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  svntest.main.file_append(mu_path, 'appended mu text')

  svntest.actions.run_and_verify_svn(None, None, [],
                                    'ci', '-m', 'changed file mu', wc_dir)

  # now checkout the repo@current_time in another folder, this should create our 
  # initial wc without the change in mu.
  checkout_target = sbox.add_wc_path('checkout')
  os.mkdir(checkout_target)

  expected_output = svntest.main.greek_state.copy()
  expected_output.wc_dir = checkout_target
  expected_output.tweak(status='A ', contents=None)
  
  expected_wc = svntest.main.greek_state.copy()

  # use an old date to checkout, that way we're sure we get the first revision
  svntest.actions.run_and_verify_checkout(sbox.repo_url + 
                                          '@{' + current_time + '}',
                                          checkout_target, 
                                          expected_output,
                                          expected_wc)

#----------------------------------------------------------------------
def co_with_obstructing_local_adds(sbox):
  "co handles obstructing paths scheduled for add"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a backup copy of the working copy
  wc_backup = sbox.add_wc_path('backup')
  svntest.actions.duplicate_dir(wc_dir, wc_backup)

  # Add files and dirs to the repos via the first WC.  Each of these
  # will be added to the backup WC via an update:
  #
  #  A/B/upsilon:   Identical to the file scheduled for addition in
  #                 the backup WC.
  #
  #  A/C/nu:        A "normal" add, won't exist in the backup WC.
  #
  #  A/D/kappa:     Conflicts with the file scheduled for addition in
  #                 the backup WC.
  #
  #  A/D/H/I:       New dirs that will also be scheduled for addition
  #  A/D/H/I/J:     in the backup WC.
  #  A/D/H/I/K:
  #
  #  A/D/H/I/L:     A "normal" dir add, won't exist in the backup WC.
  #
  #  A/D/H/I/K/xi:  Identical to the file scheduled for addition in
  #                 the backup WC.
  #
  #  A/D/H/I/K/eta: Conflicts with the file scheduled for addition in
  #                 the backup WC.
  upsilon_path = os.path.join(wc_dir, 'A', 'B', 'upsilon')
  svntest.main.file_append(upsilon_path, "This is the file 'upsilon'\n")
  nu_path = os.path.join(wc_dir, 'A', 'C', 'nu')
  svntest.main.file_append(nu_path, "This is the file 'nu'\n")
  kappa_path = os.path.join(wc_dir, 'A', 'D', 'kappa')
  svntest.main.file_append(kappa_path, "This is REPOS file 'kappa'\n")
  I_path = os.path.join(wc_dir, 'A', 'D', 'H', 'I')
  os.mkdir(I_path)
  J_path = os.path.join(I_path, 'J')
  os.mkdir(J_path)
  K_path = os.path.join(I_path, 'K')
  os.mkdir(K_path)
  L_path = os.path.join(I_path, 'L')
  os.mkdir(L_path)
  xi_path = os.path.join(K_path, 'xi')
  svntest.main.file_append(xi_path, "This is file 'xi'\n")
  eta_path = os.path.join(K_path, 'eta')
  svntest.main.file_append(eta_path, "This is REPOS file 'eta'\n")
  svntest.main.run_svn(None, 'add', upsilon_path, nu_path,
                       kappa_path, I_path)

  # Created expected output tree for 'svn ci'
  expected_output = wc.State(wc_dir, {
    'A/B/upsilon'   : Item(verb='Adding'),
    'A/C/nu'        : Item(verb='Adding'),
    'A/D/kappa'     : Item(verb='Adding'),
    'A/D/H/I'       : Item(verb='Adding'),
    'A/D/H/I/J'     : Item(verb='Adding'),
    'A/D/H/I/K'     : Item(verb='Adding'),
    'A/D/H/I/K/xi'  : Item(verb='Adding'),
    'A/D/H/I/K/eta' : Item(verb='Adding'),
    'A/D/H/I/L'     : Item(verb='Adding'),
    })

  # Create expected status tree.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'A/B/upsilon'   : Item(status='  ', wc_rev=2),
    'A/C/nu'        : Item(status='  ', wc_rev=2),
    'A/D/kappa'     : Item(status='  ', wc_rev=2),
    'A/D/H/I'       : Item(status='  ', wc_rev=2),
    'A/D/H/I/J'     : Item(status='  ', wc_rev=2),
    'A/D/H/I/K'     : Item(status='  ', wc_rev=2),
    'A/D/H/I/K/xi'  : Item(status='  ', wc_rev=2),
    'A/D/H/I/K/eta' : Item(status='  ', wc_rev=2),
    'A/D/H/I/L'     : Item(status='  ', wc_rev=2),
    })

  # Commit.
  svntest.actions.run_and_verify_commit(wc_dir, expected_output,
                                        expected_status, None,
                                        None, None, None, None, wc_dir)

  # Create various paths scheduled for addition which will obstruct
  # the adds coming from the repos.
  upsilon_backup_path = os.path.join(wc_backup, 'A', 'B', 'upsilon')
  svntest.main.file_append(upsilon_backup_path,
                           "This is the file 'upsilon'\n")
  kappa_backup_path = os.path.join(wc_backup, 'A', 'D', 'kappa')
  svntest.main.file_append(kappa_backup_path,
                           "This is WC file 'kappa'\n")
  I_backup_path = os.path.join(wc_backup, 'A', 'D', 'H', 'I')
  os.mkdir(I_backup_path)
  J_backup_path = os.path.join(I_backup_path, 'J')
  os.mkdir(J_backup_path)
  K_backup_path = os.path.join(I_backup_path, 'K')
  os.mkdir(K_backup_path)
  xi_backup_path = os.path.join(K_backup_path, 'xi')
  svntest.main.file_append(xi_backup_path, "This is file 'xi'\n")
  eta_backup_path = os.path.join(K_backup_path, 'eta')
  svntest.main.file_append(eta_backup_path, "This is WC file 'eta'\n")
  svntest.main.run_svn(None, 'add',
                       upsilon_backup_path,
                       kappa_backup_path,
                       I_backup_path)

  # Create expected output tree for an update of the wc_backup.
  expected_output = wc.State(wc_backup, {
    'A/B/upsilon'   : Item(status='E '),
    'A/C/nu'        : Item(status='A '),
    'A/D/H/I'       : Item(status='E '),
    'A/D/H/I/J'     : Item(status='E '),
    'A/D/H/I/K'     : Item(status='E '),
    'A/D/H/I/K/xi'  : Item(status='E '),
    'A/D/H/I/K/eta' : Item(status='C '),
    'A/D/H/I/L'     : Item(status='A '),
    'A/D/kappa'     : Item(status='C '),
    })

  # Create expected disk for update of wc_backup.
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.add({
    'A/B/upsilon'   : Item("This is the file 'upsilon'\n"),
    'A/C/nu'        : Item("This is the file 'nu'\n"),
    'A/D/H/I'       : Item(),
    'A/D/H/I/J'     : Item(),
    'A/D/H/I/K'     : Item(),
    'A/D/H/I/K/xi'  : Item("This is file 'xi'\n"),
    'A/D/H/I/K/eta' : Item("\n".join(["<<<<<<< .mine",
                                      "This is WC file 'eta'",
                                      "=======",
                                      "This is REPOS file 'eta'",
                                      ">>>>>>> .r2",
                                      ""])),
    'A/D/H/I/L'     : Item(),
    'A/D/kappa'     : Item("\n".join(["<<<<<<< .mine",
                                      "This is WC file 'kappa'",
                                      "=======",
                                      "This is REPOS file 'kappa'",
                                      ">>>>>>> .r2",
                                      ""])),
    })

  # Create expected status tree for the update.  Since the obstructing
  # kappa and upsilon differ from the repos, they should show as modified.
  expected_status = svntest.actions.get_virginal_state(wc_backup, 2)
  expected_status.add({
    'A/B/upsilon'   : Item(status='  ', wc_rev=2),
    'A/C/nu'        : Item(status='  ', wc_rev=2),
    'A/D/H/I'       : Item(status='  ', wc_rev=2),
    'A/D/H/I/J'     : Item(status='  ', wc_rev=2),
    'A/D/H/I/K'     : Item(status='  ', wc_rev=2),
    'A/D/H/I/K/xi'  : Item(status='  ', wc_rev=2),
    'A/D/H/I/K/eta' : Item(status='C ', wc_rev=2),
    'A/D/H/I/L'     : Item(status='  ', wc_rev=2),
    'A/D/kappa'     : Item(status='C ', wc_rev=2),
    })

  # "Extra" files that we expect to result from the conflicts.
  extra_files = ['eta\.r0', 'eta\.r2', 'eta\.mine',
                 'kappa\.r0', 'kappa\.r2', 'kappa\.mine']

  # Perform forced update and check the results in three ways.
  # We use --force here because run_and_verify_checkout() will delete
  # wc_backup before performing the checkout otherwise.
  svntest.actions.run_and_verify_checkout(sbox.repo_url, wc_backup,
                                          expected_output, expected_disk,
                                          svntest.tree.detect_conflict_files,
                                          extra_files, None, None,
                                          '--force')

  svntest.actions.run_and_verify_status(wc_backup, expected_status)

  # Some obstructions are still not permitted:
  #
  # Test that file and dir obstructions scheduled for addition *with*
  # history fail when update tries to add the same path.

  # URL to URL copy of A/D/G to A/M.
  G_URL = sbox.repo_url + '/A/D/G'
  M_URL = sbox.repo_url + '/A/M'
  svntest.actions.run_and_verify_svn("Copy error:", None, [],
                                     'cp', G_URL, M_URL, '-m', '')

  # WC to WC copy of A/D/H to A/M, M now scheduled for addition with
  # history in WC and pending addition from the repos.
  H_path = os.path.join(wc_dir, 'A', 'D', 'H')
  A_path = os.path.join(wc_dir, 'A')
  M_path = os.path.join(wc_dir, 'A', 'M')

  svntest.actions.run_and_verify_svn("Copy error:", None, [],
                                     'cp', H_path, M_path)

  # URL to URL copy of A/D/H/omega to omicron.
  omega_URL = sbox.repo_url + '/A/D/H/omega'
  omicron_URL = sbox.repo_url + '/omicron'
  svntest.actions.run_and_verify_svn("Copy error:", None, [],
                                     'cp', omega_URL, omicron_URL,
                                     '-m', '')

  # WC to WC copy of A/D/H/chi to omicron, omicron now scheduled for
  # addition with history in WC and pending addition from the repos.
  chi_path = os.path.join(wc_dir, 'A', 'D', 'H', 'chi')
  omicron_path = os.path.join(wc_dir, 'omicron')

  svntest.actions.run_and_verify_svn("Copy error:", None, [],
                                     'cp', chi_path,
                                     omicron_path)

  # Try to co M's Parent.
  sout, serr = svntest.actions.run_and_verify_svn("Checkout XPASS",
                                                  [], SVNAnyOutput,
                                                  'co', sbox.repo_url + '/A',
                                                  A_path)

  test_stderr("svn: Failed to add directory '.*M': a versioned " \
              "directory of the same name already exists\n", serr)

  # --force shouldn't help either.
  sout, serr = svntest.actions.run_and_verify_svn("Checkout XPASS",
                                                  [], SVNAnyOutput,
                                                  'co', sbox.repo_url + '/A',
                                                  A_path, '--force')

  test_stderr("svn: Failed to add directory '.*M': a versioned " \
              "directory of the same name already exists\n", serr)

  # Try to co omicron's parent, non-recusively so as not to
  # try and update M first.
  sout, serr = svntest.actions.run_and_verify_svn("Checkout XPASS",
                                                  [], SVNAnyOutput,
                                                  'co', sbox.repo_url,
                                                  wc_dir, '-N')

  test_stderr("svn: Failed to add file '.*omicron': a file of the same " \
              "name is already scheduled for addition with history\n", serr)

  # Again, --force shouldn't matter.
  sout, serr = svntest.actions.run_and_verify_svn("Checkout XPASS",
                                                  [], SVNAnyOutput,
                                                  'co', sbox.repo_url,
                                                  wc_dir, '-N', '--force')

  test_stderr("svn: Failed to add file '.*omicron': a file of the same " \
              "name is already scheduled for addition with history\n", serr)

#----------------------------------------------------------------------

# list all tests here, starting with None:
test_list = [ None,
              checkout_with_obstructions,
              forced_checkout_of_file_with_dir_obstructions,
              forced_checkout_of_dir_with_file_obstructions,
              forced_checkout_with_faux_obstructions,
              forced_checkout_with_real_obstructions,
              forced_checkout_with_real_obstructions_and_unversioned_files,
              forced_checkout_with_versioned_obstruction,
              import_and_checkout,
              checkout_broken_eol,
              checkout_creates_intermediate_folders,
              checkout_peg_rev,
              checkout_peg_rev_date,
              co_with_obstructing_local_adds,
            ]

if __name__ == "__main__":
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

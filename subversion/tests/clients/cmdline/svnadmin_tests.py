#!/usr/bin/env python
#
#  svnadmin_tests.py:  testing the 'svnadmin' tool.
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2001 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import shutil, string, sys, re, os.path

# The `svntest' module
import svntest

# Quick macro for auto-generating sandbox names
def sandbox(x):
  return "svnadmin_tests-" + `test_list.index(x)`

# (abbreviation)
path_index = svntest.actions.path_index

#----------------------------------------------------------------------

# How we currently test 'svnadmin' --
#
#   'svnadmin create':   This subcommand is implicitly tested
#                        every time we run a python test script!
#                        (An empty repository is always created and then
#                        imported into;  if this subcommand stopped working,
#                        every test would fail and we would know instantly.)
#
#   'svnadmin youngest': Just commit a couple of times and directly parse
#                        the printed number.
#
#   'svnadmin createtxn'
#   'svnadmin rmtxn':    See below.
#
#   'svnadmin lstxns':   We don't care about the contents of transactions;
#                        we only care that they exist or not.
#                        Therefore, we can simply parse transaction headers.
#
#   'svnadmin lsrevs':   Parse headers as above.
#
#  ### TODO:  someday maybe we could parse the contents of trees too.
#
######################################################################
# Helper routines


def get_trees(repo_dir, revision_p = 0):
  """Run 'svnadmin' on REPO_DIR, with a subcommand of either 'lstxns'
  or 'lsrevs', depending on the state of REVISION_P.  Return a list of
  strings that are either transaction names or revision numbers."""

  tree_names = []

  if revision_p:
    output_lines, errput_lines = svntest.main.run_svnadmin("lsrevs", repo_dir)
    rm = re.compile("^Revision\s+(.+)")
  else:
    output_lines, errput_lines = svntest.main.run_svnadmin("lstxns", repo_dir)
    rm = re.compile("^Txn\s+(.+):")

  for line in output_lines:
    match = rm.search(line)
    if match and match.groups():
      tree_names.append(match.group(1))
      
  return tree_names




######################################################################
# Tests
#
#   Each test must return 0 on success or non-zero on failure.


#----------------------------------------------------------------------

def test_youngest():
  "test 'svnadmin youngest' subcommand"

  # Bootstrap
  sbox = sandbox(test_youngest)
  wc_dir = os.path.join (svntest.main.general_wc_dir, sbox)
  repo_dir = os.path.join (svntest.main.general_repo_dir, sbox)

  if svntest.actions.make_repo_and_wc(sbox):
    return 1

  # Make a couple of local mods to files
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  svntest.main.file_append (mu_path, 'appended mu text')
  svntest.main.file_append (rho_path, 'new appended text for rho')

  # Created expected output tree for 'svn ci'
  output_list = [ [mu_path, None, {}, {'verb' : 'Changing' }],
                  [rho_path, None, {}, {'verb' : 'Changing' }] ]
  expected_output_tree = svntest.tree.build_generic_tree(output_list)

  # Create expected status tree; all local revisions should be at 1,
  # but mu and rho should be at revision 2.
  status_list = svntest.actions.get_virginal_status_list(wc_dir, '2')
  for item in status_list:
    if (item[0] != mu_path) and (item[0] != rho_path):
      item[3]['wc_rev'] = '1'
  expected_status_tree = svntest.tree.build_generic_tree(status_list)

  if svntest.actions.run_and_verify_commit (wc_dir,
                                            expected_output_tree,
                                            expected_status_tree,
                                            None,
                                            None, None,
                                            None, None,
                                            wc_dir):
    return 1

  # Youngest revision should now be 2.  Let's verify that.
  output, errput = svntest.main.run_svnadmin("youngest", repo_dir)

  if output[0] != "2\n":
    return 1

  return 0  # success

#----------------------------------------------------------------------

def create_txn():
  "test 'svnadmin createtxn' subcommand"

  # Bootstrap
  sbox = sandbox(create_txn)
  wc_dir = os.path.join (svntest.main.general_wc_dir, sbox)
  repo_dir = os.path.join (svntest.main.general_repo_dir, sbox)

  if svntest.actions.make_repo_and_wc(sbox):
    return 1

  # Make a transaction based on revision 1.
  output, errput = svntest.main.run_svnadmin("createtxn", repo_dir, "1")

  # Look for it by running 'lstxn'.
  tree_list = get_trees(repo_dir)
  if tree_list != ['1']:
    return 1

  return 0


#----------------------------------------------------------------------

def remove_txn():
  "test 'svnadmin rmtxn' subcommand"

  # Bootstrap
  sbox = sandbox(remove_txn)
  wc_dir = os.path.join (svntest.main.general_wc_dir, sbox)
  repo_dir = os.path.join (svntest.main.general_repo_dir, sbox)

  if svntest.actions.make_repo_and_wc(sbox):
    return 1

  # Make three transactions based on revision 1.
  svntest.main.run_svnadmin("createtxn", repo_dir, "1")
  svntest.main.run_svnadmin("createtxn", repo_dir, "1")
  svntest.main.run_svnadmin("createtxn", repo_dir, "1")

  # Look for them by running 'lstxn'.
  tree_list = get_trees(repo_dir)
  if tree_list != ['1', '2', '3']:
    return 1

  # Remove the 2nd transaction.
  svntest.main.run_svnadmin("rmtxn", repo_dir, "2")

  # Examine the list of transactions again.
  tree_list = get_trees(repo_dir)
  if tree_list != ['1', '3']:
    return 1

  return 0

#----------------------------------------------------------------------

def list_revs():
  "test 'svnadmin lsrevs' subcommand"

  # Bootstrap
  sbox = sandbox(list_revs)
  wc_dir = os.path.join (svntest.main.general_wc_dir, sbox)
  repo_dir = os.path.join (svntest.main.general_repo_dir, sbox)

  if svntest.actions.make_repo_and_wc(sbox):
    return 1

  # Make a couple of local mods to files
  mu_path = os.path.join(wc_dir, 'A', 'mu')
  rho_path = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')
  svntest.main.file_append (mu_path, 'appended mu text')
  svntest.main.file_append (rho_path, 'new appended text for rho')

  # Created expected output tree for 'svn ci'
  output_list = [ [mu_path, None, {}, {'verb' : 'Changing' }],
                  [rho_path, None, {}, {'verb' : 'Changing' }] ]
  expected_output_tree = svntest.tree.build_generic_tree(output_list)

  # Create expected status tree; all local revisions should be at 1,
  # but mu and rho should be at revision 2.
  status_list = svntest.actions.get_virginal_status_list(wc_dir, '2')
  for item in status_list:
    if (item[0] != mu_path) and (item[0] != rho_path):
      item[3]['wc_rev'] = '1'
  expected_status_tree = svntest.tree.build_generic_tree(status_list)

  # Commit, and create revision 2.
  if svntest.actions.run_and_verify_commit (wc_dir,
                                            expected_output_tree,
                                            expected_status_tree,
                                            None,
                                            None, None,
                                            None, None,
                                            wc_dir):
    return 1

  # Remove gamma
  gamma_path = os.path.join(wc_dir, 'A', 'D', 'gamma')
  svntest.main.run_svn('rm', gamma_path)

  # Created expected output tree for 'svn ci'
  output_list = [ [gamma_path, None, {}, {'verb' : 'Deleting' }] ]
  expected_output_tree = svntest.tree.build_generic_tree(output_list)

  # Create expected status tree
  status_list = svntest.actions.get_virginal_status_list(wc_dir, '3')
  for item in status_list:
    item[3]['wc_rev'] = '1'
    if (item[0] == mu_path) or (item[0] == rho_path):
      item[3]['wc_rev'] = '2'
  status_list.pop(path_index(status_list, gamma_path))
  expected_status_tree = svntest.tree.build_generic_tree(status_list)

  # Commit, and create revision 3.
  if svntest.actions.run_and_verify_commit (wc_dir,
                                            expected_output_tree,
                                            expected_status_tree,
                                            None,
                                            None, None,
                                            None, None,
                                            wc_dir):
    return 1

  # Now fetch all revisions from the repository.
  tree_list = get_trees(repo_dir, 1)
  if tree_list != ['0', '1', '2', '3']:
    return 1

  return 0



########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              test_youngest,
              create_txn,
              remove_txn,
              list_revs
             ]

if __name__ == '__main__':
  
  ## run the main test routine on them:
  err = svntest.main.run_tests(test_list)

  ## remove all scratchwork: the 'pristine' repository, greek tree, etc.
  ## This ensures that an 'import' will happen the next time we run.
  if os.path.exists(svntest.main.temp_dir):
    shutil.rmtree(svntest.main.temp_dir)

  ## return whatever main() returned to the OS.
  sys.exit(err)


### End of file.
# local variables:
# eval: (load-file "../../../svn-dev.el")
# end:

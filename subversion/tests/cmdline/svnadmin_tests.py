#!/usr/bin/env python
#
#  svnadmin_tests.py:  testing the 'svnadmin' tool.
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
import shutil
import sys

# Our testing module
import svntest
from svntest.verify import SVNExpectedStdout, SVNExpectedStderr

# (abbreviation)
Skip = svntest.testcase.Skip
SkipUnless = svntest.testcase.SkipUnless
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem


#----------------------------------------------------------------------

# How we currently test 'svnadmin' --
#
#   'svnadmin create':   Create an empty repository, test that the
#                        root node has a proper created-revision,
#                        because there was once a bug where it
#                        didn't.
#
#                        Note also that "svnadmin create" is tested
#                        implicitly every time we run a python test
#                        script.  (An empty repository is always
#                        created and then imported into;  if this
#                        subcommand failed catastrophically, every
#                        test would fail and we would know instantly.)
#
#   'svnadmin createtxn'
#   'svnadmin rmtxn':    See below.
#
#   'svnadmin lstxns':   We don't care about the contents of transactions;
#                        we only care that they exist or not.
#                        Therefore, we can simply parse transaction headers.
#
#   'svnadmin dump':     A couple regression tests that ensure dump doesn't
#                        error out, and one to check that the --quiet option
#                        really does what it's meant to do. The actual
#                        contents of the dump aren't verified at all.
#
#  ### TODO:  someday maybe we could parse the contents of trees too.
#
######################################################################
# Helper routines


def get_txns(repo_dir):
  "Get the txn names using 'svnadmin lstxns'."

  output_lines, error_lines = svntest.main.run_svnadmin('lstxns', repo_dir)
  txns = map(output_lines.strip, output_lines)

  # sort, just in case
  txns.sort()

  return txns

def load_and_verify_dumpstream(sbox, expected_stdout, expected_stderr,
                               revs, dump, *varargs):
  """Load the array of lines passed in 'dump' into the
  current tests' repository and verify the repository content
  using the array of wc.States passed in revs. VARARGS are optional
  arguments passed to the 'load' command"""

  if type(dump) is type(""):
    dump = [ dump ]

  output, errput = \
          svntest.main.run_command_stdin(
    svntest.main.svnadmin_binary, expected_stderr, 1, dump,
    'load', '--quiet', sbox.repo_dir, *varargs)

  if expected_stdout:
    if expected_stdout == svntest.verify.AnyOutput:
      if len(output) == 0:
        raise SVNExpectedStdout
    else:
      svntest.verify.compare_and_display_lines(
        "Standard output", "STDOUT:", expected_stdout, output)

  if expected_stderr:
    if expected_stderr == svntest.verify.AnyOutput:
      if len(errput) == 0:
        raise SVNExpectedStderr
    else:
      svntest.verify.compare_and_display_lines(
        "Standard error output", "STDERR:", expected_stderr, errput)
    # The expected error occurred, so don't try to verify the result
    return

  if revs:
    # verify revs as wc states
    for rev in xrange(len(revs)):
      svntest.actions.run_and_verify_svn("Updating to r%s" % (rev+1),
                                         svntest.verify.AnyOutput, [],
                                         "update", "-r%s" % (rev+1),
                                         sbox.wc_dir)

      wc_tree = svntest.tree.build_tree_from_wc(sbox.wc_dir)
      rev_tree = revs[rev].old_tree()

      try:
        svntest.tree.compare_trees ("rev/disk", rev_tree, wc_tree)
      except svntest.tree.SVNTreeError:
        svntest.verify.display_trees(None, 'WC TREE', wc_tree, rev_tree)
        raise


######################################################################
# Tests


#----------------------------------------------------------------------

def test_create(sbox):
  "'svnadmin create'"


  repo_dir = sbox.repo_dir
  wc_dir = sbox.wc_dir

  svntest.main.safe_rmtree(repo_dir)
  svntest.main.safe_rmtree(wc_dir)

  svntest.main.create_repos(repo_dir)

  svntest.actions.run_and_verify_svn("Creating rev 0 checkout",
                                     ["Checked out revision 0.\n"], [],
                                     "checkout",
                                     sbox.repo_url, wc_dir)


  svntest.actions.run_and_verify_svn(
    "Running status",
    [], [],
    "status", wc_dir)

  svntest.actions.run_and_verify_svn(
    "Running verbose status",
    ["                0        0  ?           %s\n" % wc_dir], [],
    "status", "--verbose", wc_dir)

  # success


# dump stream tests need a dump file

def clean_dumpfile():
  return \
  [ "SVN-fs-dump-format-version: 2\n\n",
    "UUID: 668cc64a-31ed-0310-8ccb-b75d75bb44e3\n\n",
    "Revision-number: 0\n",
    "Prop-content-length: 56\n",
    "Content-length: 56\n\n",
    "K 8\nsvn:date\nV 27\n2005-01-08T21:48:13.838745Z\nPROPS-END\n\n\n",
    "Revision-number: 1\n",
    "Prop-content-length: 98\n",
    "Content-length: 98\n\n",
    "K 7\nsvn:log\nV 0\n\nK 10\nsvn:author\nV 4\nerik\n",
    "K 8\nsvn:date\nV 27\n2005-01-08T21:51:16.313791Z\nPROPS-END\n\n\n",
    "Node-path: A\n",
    "Node-kind: file\n",
    "Node-action: add\n",
    "Prop-content-length: 35\n",
    "Text-content-length: 5\n",
    "Text-content-md5: e1cbb0c3879af8347246f12c559a86b5\n",
    "Content-length: 40\n\n",
    "K 12\nsvn:keywords\nV 2\nId\nPROPS-END\ntext\n\n\n"]

dumpfile_revisions = \
  [ svntest.wc.State('', { 'A' : svntest.wc.StateItem(contents="text\n") }) ]

#----------------------------------------------------------------------
def extra_headers(sbox):
  "loading of dumpstream with extra headers"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  dumpfile[3:3] = \
       [ "X-Comment-Header: Ignored header normally not in dump stream\n" ]

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, dumpfile)

#----------------------------------------------------------------------
# Ensure loading continues after skipping a bit of unknown extra content.
def extra_blockcontent(sbox):
  "load success on oversized Content-length"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  # Replace "Content-length" line with two lines
  dumpfile[8:9] = \
       [ "Extra-content-length: 10\n",
         "Content-length: 108\n\n" ]
  # Insert the extra content after "PROPS-END\n"
  dumpfile[11] = dumpfile[11][:-2] + "extra text\n\n\n"

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, dumpfile)

#----------------------------------------------------------------------
def inconsistent_headers(sbox):
  "load failure on undersized Content-length"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  dumpfile[-2] = "Content-length: 30\n\n"

  load_and_verify_dumpstream(sbox, [], svntest.verify.AnyOutput,
                             dumpfile_revisions, dumpfile)

#----------------------------------------------------------------------
# Test for issue #2729: Datestamp-less revisions in dump streams do
# not remain so after load
def empty_date(sbox):
  "preserve date-less revisions in load (issue #2729)"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  # Replace portions of the revision data to drop the svn:date revprop.
  dumpfile[7:11] = \
       [ "Prop-content-length: 52\n",
         "Content-length: 52\n\n",
         "K 7\nsvn:log\nV 0\n\nK 10\nsvn:author\nV 4\nerik\nPROPS-END\n\n\n"
         ]

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, dumpfile)

  # Verify that the revision still lacks the svn:date property.
  svntest.actions.run_and_verify_svn(None, [], [], "propget",
                                     "--revprop", "-r1", "svn:date",
                                     sbox.wc_dir)

#----------------------------------------------------------------------

def dump_copied_dir(sbox):
  "'svnadmin dump' on copied directory"

  sbox.build()
  wc_dir = sbox.wc_dir
  repo_dir = sbox.repo_dir

  old_C_path = os.path.join(wc_dir, 'A', 'C')
  new_C_path = os.path.join(wc_dir, 'A', 'B', 'C')
  svntest.main.run_svn(None, 'cp', old_C_path, new_C_path)
  svntest.main.run_svn(None, 'ci', wc_dir, '--quiet',
                       '-m', 'log msg')

  output, errput = svntest.main.run_svnadmin("dump", repo_dir)
  if svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput):
    raise svntest.Failure

#----------------------------------------------------------------------

def dump_move_dir_modify_child(sbox):
  "'svnadmin dump' on modified child of copied dir"

  sbox.build()
  wc_dir = sbox.wc_dir
  repo_dir = sbox.repo_dir

  B_path = os.path.join(wc_dir, 'A', 'B')
  Q_path = os.path.join(wc_dir, 'A', 'Q')
  svntest.main.run_svn(None, 'cp', B_path, Q_path)
  svntest.main.file_append(os.path.join(Q_path, 'lambda'), 'hello')
  svntest.main.run_svn(None, 'ci', wc_dir, '--quiet',
                       '-m', 'log msg')
  output, errput = svntest.main.run_svnadmin("dump", repo_dir)
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput)

  output, errput = svntest.main.run_svnadmin("dump", "-r", "0:HEAD", repo_dir)
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput)

#----------------------------------------------------------------------

def dump_quiet(sbox):
  "'svnadmin dump --quiet'"

  sbox.build(create_wc = False)

  output, errput = svntest.main.run_svnadmin("dump", sbox.repo_dir, '--quiet')
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump --quiet' is unexpected.",
    'STDERR', [], errput)

#----------------------------------------------------------------------

def hotcopy_dot(sbox):
  "'svnadmin hotcopy PATH .'"
  sbox.build()

  backup_dir, backup_url = sbox.add_repo_path('backup')
  os.mkdir(backup_dir)
  cwd = os.getcwd()

  os.chdir(backup_dir)
  output, errput = svntest.main.run_svnadmin("hotcopy",
                                             os.path.join(cwd, sbox.repo_dir),
                                             '.')
  if errput:
    raise svntest.Failure

  os.chdir(cwd)

  origout, origerr = svntest.main.run_svnadmin("dump", sbox.repo_dir, '--quiet')
  backout, backerr = svntest.main.run_svnadmin("dump", backup_dir, '--quiet')
  if origerr or backerr or origout != backout:
    raise svntest.Failure

#----------------------------------------------------------------------

def hotcopy_format(sbox):
  "'svnadmin hotcopy' checking db/format file"
  sbox.build()

  backup_dir, backup_url = sbox.add_repo_path('backup')
  output, errput = svntest.main.run_svnadmin("hotcopy", sbox.repo_dir,
                                             backup_dir)
  if errput:
    print "Error: hotcopy failed"
    raise svntest.Failure

  # verify that the db/format files are the same
  fp = open(os.path.join(sbox.repo_dir, "db", "format"))
  contents1 = fp.read()
  fp.close()

  fp2 = open(os.path.join(backup_dir, "db", "format"))
  contents2 = fp2.read()
  fp2.close()

  if contents1 != contents2:
    print "Error: db/format file contents do not match after hotcopy"
    raise svntest.Failure

#----------------------------------------------------------------------

def setrevprop(sbox):
  "'setlog' and 'setrevprop', bypassing hooks'"
  sbox.build()

  # Try a simple log property modification.
  iota_path = os.path.join(sbox.wc_dir, "iota")
  output, errput = svntest.main.run_svnadmin("setlog", sbox.repo_dir,
                                             "-r0", "--bypass-hooks",
                                             iota_path)
  if errput:
    print "Error: 'setlog' failed"
    raise svntest.Failure

  # Verify that the revprop value matches what we set when retrieved
  # through the client.
  svntest.actions.run_and_verify_svn(None,
                                     [ "This is the file 'iota'.\n", "\n" ],
                                     [], "propget", "--revprop", "-r0",
                                     "svn:log", sbox.wc_dir)

  # Try an author property modification.
  foo_path = os.path.join(sbox.wc_dir, "foo")
  svntest.main.file_write(foo_path, "foo")

  output, errput = svntest.main.run_svnadmin("setrevprop", sbox.repo_dir,
                                             "-r0", "svn:author", foo_path)
  if errput:
    print "Error: 'setrevprop' failed"
    raise svntest.Failure

  # Verify that the revprop value matches what we set when retrieved
  # through the client.
  svntest.actions.run_and_verify_svn(None, [ "foo\n" ], [], "propget",
                                     "--revprop", "-r0", "svn:author",
                                     sbox.wc_dir)

def verify_windows_paths_in_repos(sbox):
  "verify a repository containing paths like 'c:hi'"

  # setup a repo with a directory 'c:hi'
  sbox.build(create_wc = False)
  repo_url       = sbox.repo_url
  chi_url = sbox.repo_url + '/c:hi'

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir', '-m', 'log_msg',
                                     chi_url)

  output, errput = svntest.main.run_svnadmin("verify", sbox.repo_dir)
  svntest.verify.compare_and_display_lines(
    "Error while running 'svnadmin verify'.",
    'STDERR', ["* Verified revision 0.\n",
               "* Verified revision 1.\n",
               "* Verified revision 2.\n"], errput)

#----------------------------------------------------------------------

def recover_fsfs(sbox):
  "recover a repository (FSFS only)"

  # Set up a repository containing the greek tree.
  sbox.build(create_wc = False)

  # Read the current contents of the current file.
  current_path = os.path.join(sbox.repo_dir, 'db', 'current')
  expected_current_contents = svntest.main.file_read(current_path)

  # Remove the current file.
  os.remove(current_path)

  # Run 'svnadmin recover' and check that the current file is recreated.
  output, errput = svntest.main.run_svnadmin("recover", sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr

  actual_current_contents = svntest.main.file_read(current_path)
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

#----------------------------------------------------------------------

def load_with_parent_dir(sbox):
  "'svnadmin load --parent-dir' reparents mergeinfo"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2983. ##
  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svnadmin_tests_data',
                                   'mergeinfo_included.dump')
  dumpfile = svntest.main.file_read(dumpfile_location)

  # Create 'sample' dir in sbox.repo_url
  svntest.actions.run_and_verify_svn(None,
                                     ['\n', 'Committed revision 1.\n'],
                                     [], "mkdir", sbox.repo_url + "/sample",
                                     "-m", "Create sample dir")

  # Load the dump stream
  load_and_verify_dumpstream(sbox,[],[], None, dumpfile, '--parent-dir',
                             '/sample')

  # Verify the svn:mergeinfo properties for '--parent-dir'
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample/branch - /sample/trunk:5-7\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample/branch')
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample/branch1 - " +
                                      "/sample/branch:6-9\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample/branch1')

#----------------------------------------------------------------------

def set_uuid(sbox):
  "test 'svnadmin setuuid'"

  sbox.build(create_wc=False)

  # Squirrel away the original repository UUID.
  output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr
  orig_uuid = output[0].rstrip()

  # Try setting a new, bogus UUID.
  svntest.actions.run_and_verify_svnadmin(None, None, '^.*Malformed UUID.*$',
                                          'setuuid', sbox.repo_dir, 'abcdef')

  # Try generating a brand new UUID.
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', sbox.repo_dir)
  output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr
  new_uuid = output[0].rstrip()
  if new_uuid == orig_uuid:
    print "Error: new UUID matches the original one"
    raise svntest.Failure

  # Now, try setting the UUID back to the original value.
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', sbox.repo_dir, orig_uuid)
  output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr
  new_uuid = output[0].rstrip()
  if new_uuid != orig_uuid:
    print "Error: new UUID doesn't match the original one"
    raise svntest.Failure

#----------------------------------------------------------------------

def reflect_dropped_renumbered_revs(sbox):
  "reflect dropped renumbered revs in svn:mergeinfo"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=3020. ##

  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'with_merges.dump')
  dumpfile = svntest.main.file_read(dumpfile_location)

  # Create 'toplevel' dir in sbox.repo_url
  svntest.actions.run_and_verify_svn(None, ['\n', 'Committed revision 1.\n'],
                                     [], "mkdir", sbox.repo_url + "/toplevel",
                                     "-m", "Create toplevel dir")

  # Load the dump stream in sbox.repo_url
  load_and_verify_dumpstream(sbox,[],[], None, dumpfile)

  # Load the dump stream in toplevel dir
  load_and_verify_dumpstream(sbox,[],[], None, dumpfile, '--parent-dir',
                             '/toplevel')

  # Verify the svn:mergeinfo properties
  svntest.actions.run_and_verify_svn(None, ["/trunk:1-4\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/branch2')
  svntest.actions.run_and_verify_svn(None, ["/branch1:5-9\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/trunk')
  svntest.actions.run_and_verify_svn(None, ["/toplevel/trunk:1-13\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/toplevel/branch2')
  svntest.actions.run_and_verify_svn(None, ["/toplevel/branch1:14-18\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/toplevel/trunk')
  svntest.actions.run_and_verify_svn(None, ["/toplevel/trunk:1-12\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/toplevel/branch1')
  svntest.actions.run_and_verify_svn(None, ["/trunk:1-3\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/branch1')

#----------------------------------------------------------------------

def fsfs_recover_handle_missing_revs_or_revprops_file(sbox):
  """fsfs recovery checks missing revs / revprops files"""
  # Set up a repository containing the greek tree.
  sbox.build(create_wc = False)

  # Remove the revs file for revision 1
  os.remove(os.path.join(sbox.repo_dir, 'db','revs','0', '1'));
  
  # Verify 'svnadmin recover' fails when youngest has a revprops
  # file but no revs file
  output, errput = svntest.main.run_svnadmin("recover", sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.",
    None,
    errput,
    None,
    ".*Expected youngest rev to be 1 but found 0"):
    raise svntest.Failure

  # Remove previous repository and recreate
  shutil.rmtree(sbox.repo_dir)
  sbox.build(create_wc = False)

  # Remove the revprops file for revision 1
  os.remove(os.path.join(sbox.repo_dir, 'db','revprops','0','1'));

  # Verify 'svnadmin recover' fails when youngest has a revs file
  # but no revprops file (issue #2992).
  output, errput = svntest.main.run_svnadmin("recover", sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.",
    None,
    errput,
    None,
    ".*Revision 1 has a revs file but no revprops file"):
    raise svntest.Failure

  # Remove previous repository and recreate
  shutil.rmtree(sbox.repo_dir)
  sbox.build(create_wc = False)

  # Change revprops file to a directory for revision 1
  os.remove(os.path.join(sbox.repo_dir, 'db','revprops','0','1'));
  os.mkdir(os.path.join(sbox.repo_dir, 'db','revprops','0','1'));

  # Verify 'svnadmin recover' fails when youngest has a revs file
  # but no revprops file (another aspect of issue #2992).
  output, errput = svntest.main.run_svnadmin("recover", sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.",
    None,
    errput,
    None,
    ".*Revision 1 has a non-file where its revprops file should be.*"):
    raise svntest.Failure

########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              extra_headers,
              extra_blockcontent,
              inconsistent_headers,
              empty_date,
              dump_copied_dir,
              dump_move_dir_modify_child,
              dump_quiet,
              hotcopy_dot,
              hotcopy_format,
              setrevprop,
              verify_windows_paths_in_repos,
              SkipUnless(recover_fsfs, svntest.main.is_fs_type_fsfs),
              load_with_parent_dir,
              set_uuid,
              reflect_dropped_renumbered_revs,
              SkipUnless(fsfs_recover_handle_missing_revs_or_revprops_file,
                         svntest.main.is_fs_type_fsfs),
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

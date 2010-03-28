#!/usr/bin/env python
#
#  svnadmin_tests.py:  testing the 'svnadmin' tool.
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
import os
import shutil
import sys

# Our testing module
import svntest
from svntest.verify import SVNExpectedStdout, SVNExpectedStderr
from svntest.verify import SVNUnexpectedStderr
from svntest.main import SVN_PROP_MERGEINFO

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

  exit_code, output_lines, error_lines = svntest.main.run_svnadmin('lstxns',
                                                                   repo_dir)
  txns = sorted([output_lines.strip(x) for x in output_lines])

  return txns

def load_and_verify_dumpstream(sbox, expected_stdout, expected_stderr,
                               revs, dump, *varargs):
  """Load the array of lines passed in 'dump' into the
  current tests' repository and verify the repository content
  using the array of wc.States passed in revs. VARARGS are optional
  arguments passed to the 'load' command"""

  if isinstance(dump, str):
    dump = [ dump ]

  exit_code, output, errput = svntest.main.run_command_stdin(
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
    for rev in range(len(revs)):
      svntest.actions.run_and_verify_svn("Updating to r%s" % (rev+1),
                                         svntest.verify.AnyOutput, [],
                                         "update", "-r%s" % (rev+1),
                                         sbox.wc_dir)

      wc_tree = svntest.tree.build_tree_from_wc(sbox.wc_dir)
      rev_tree = revs[rev].old_tree()

      try:
        svntest.tree.compare_trees("rev/disk", rev_tree, wc_tree)
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

  svntest.main.safe_rmtree(repo_dir, 1)
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
    ["                 0        0  ?           %s\n" % wc_dir], [],
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

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, dumpfile,
                             '--ignore-uuid')

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

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, dumpfile,
                             '--ignore-uuid')

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

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, dumpfile,
                             '--ignore-uuid')

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

  exit_code, output, errput = svntest.main.run_svnadmin("dump", repo_dir)
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
  exit_code, output, errput = svntest.main.run_svnadmin("dump", repo_dir)
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput)

  exit_code, output, errput = svntest.main.run_svnadmin("dump", "-r",
                                                        "0:HEAD", repo_dir)
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput)

#----------------------------------------------------------------------

def dump_quiet(sbox):
  "'svnadmin dump --quiet'"

  sbox.build(create_wc = False)

  exit_code, output, errput = svntest.main.run_svnadmin("dump", sbox.repo_dir,
                                                        '--quiet')
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
  svntest.actions.run_and_verify_svnadmin(
    None, None, [],
    "hotcopy", os.path.join(cwd, sbox.repo_dir), '.')

  os.chdir(cwd)

  exit_code, origout, origerr = svntest.main.run_svnadmin("dump",
                                                          sbox.repo_dir,
                                                          '--quiet')
  exit_code, backout, backerr = svntest.main.run_svnadmin("dump",
                                                          backup_dir,
                                                          '--quiet')
  if origerr or backerr or origout != backout:
    raise svntest.Failure

#----------------------------------------------------------------------

def hotcopy_format(sbox):
  "'svnadmin hotcopy' checking db/format file"
  sbox.build()

  backup_dir, backup_url = sbox.add_repo_path('backup')
  exit_code, output, errput = svntest.main.run_svnadmin("hotcopy",
                                                        sbox.repo_dir,
                                                        backup_dir)
  if errput:
    print("Error: hotcopy failed")
    raise svntest.Failure

  # verify that the db/format files are the same
  fp = open(os.path.join(sbox.repo_dir, "db", "format"))
  contents1 = fp.read()
  fp.close()

  fp2 = open(os.path.join(backup_dir, "db", "format"))
  contents2 = fp2.read()
  fp2.close()

  if contents1 != contents2:
    print("Error: db/format file contents do not match after hotcopy")
    raise svntest.Failure

#----------------------------------------------------------------------

def setrevprop(sbox):
  "'setlog' and 'setrevprop', bypassing hooks'"
  sbox.build()

  # Try a simple log property modification.
  iota_path = os.path.join(sbox.wc_dir, "iota")
  exit_code, output, errput = svntest.main.run_svnadmin("setlog",
                                                        sbox.repo_dir,
                                                        "-r0",
                                                        "--bypass-hooks",
                                                        iota_path)
  if errput:
    print("Error: 'setlog' failed")
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

  exit_code, output, errput = svntest.main.run_svnadmin("setrevprop",
                                                        sbox.repo_dir,
                                                        "-r0", "svn:author",
                                                        foo_path)
  if errput:
    print("Error: 'setrevprop' failed")
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

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)
  svntest.verify.compare_and_display_lines(
    "Error while running 'svnadmin verify'.",
    'STDERR', ["* Verified revision 0.\n",
               "* Verified revision 1.\n",
               "* Verified revision 2.\n"], errput)

#----------------------------------------------------------------------

# Returns the filename of the rev or revprop file (according to KIND)
# numbered REV in REPO_DIR, which must be in the first shard if we're
# using a sharded repository.
def fsfs_file(repo_dir, kind, rev):
  if svntest.main.server_minor_version >= 5:
    if svntest.main.fsfs_sharding is None:
      return os.path.join(repo_dir, 'db', kind, '0', rev)
    else:
      shard = int(rev) // svntest.main.fsfs_sharding
      path = os.path.join(repo_dir, 'db', kind, str(shard), rev)

      if svntest.main.fsfs_packing is None or kind == 'revprops':
        # we don't pack revprops
        return path
      elif os.path.exists(path):
        # rev exists outside a pack file.
        return path
      else:
        # didn't find the plain file; assume it's in a pack file
        return os.path.join(repo_dir, 'db', kind, ('%d.pack' % shard), 'pack')
  else:
    return os.path.join(repo_dir, 'db', kind, rev)


def verify_incremental_fsfs(sbox):
  """svnadmin verify detects corruption dump can't"""

  # setup a repo with a directory 'c:hi'
  sbox.build(create_wc = False)
  repo_url = sbox.repo_url
  E_url = sbox.repo_url + '/A/B/E'

  # Create A/B/E/bravo in r2.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir', '-m', 'log_msg',
                                     E_url + '/bravo')
  # Corrupt r2's reference to A/C by replacing "dir 7-1.0.r1/1568" with
  # "dir 7-1.0.r1/1569" (increment offset) and updating the checksum for
  # this directory listing to "c9b5a2d26473a4e28088673dda9df804" so that
  # the listing itself is valid.
  r2 = fsfs_file(sbox.repo_dir, 'revs', '2')
  if r2.endswith('pack'):
    raise svntest.Skip

  fp = open(r2, 'wb')
  fp.write("""id: 0-2.0.r2/0
type: dir
count: 0
cpath: /A/B/E/bravo
copyroot: 0 /

PLAIN
K 5
alpha
V 17
file 3-1.0.r1/719
K 4
beta
V 17
file 4-1.0.r1/840
K 5
bravo
V 14
dir 0-2.0.r2/0
END
ENDREP
id: 2-1.0.r2/181
type: dir
pred: 2-1.0.r1/1043
count: 1
text: 2 69 99 99 f63001f7fddd1842d8891474d0982111
cpath: /A/B/E
copyroot: 0 /

PLAIN
K 1
E
V 16
dir 2-1.0.r2/181
K 1
F
V 17
dir 5-1.0.r1/1160
K 6
lambda
V 17
file 6-1.0.r1/597
END
ENDREP
id: 1-1.0.r2/424
type: dir
pred: 1-1.0.r1/1335
count: 1
text: 2 316 95 95 bccb66379b4f825dac12b50d80211bae
cpath: /A/B
copyroot: 0 /

PLAIN
K 1
B
V 16
dir 1-1.0.r2/424
K 1
C
V 17
dir 7-1.0.r1/1569
K 1
D
V 17
dir 8-1.0.r1/3061
K 2
mu
V 18
file i-1.0.r1/1451
END
ENDREP
id: 0-1.0.r2/692
type: dir
pred: 0-1.0.r1/3312
count: 1
text: 2 558 121 121 c9b5a2d26473a4e28088673dda9df804
cpath: /A
copyroot: 0 /

PLAIN
K 1
A
V 16
dir 0-1.0.r2/692
K 4
iota
V 18
file j-1.0.r1/3428
END
ENDREP
id: 0.0.r2/904
type: dir
pred: 0.0.r1/3624
count: 2
text: 2 826 65 65 e44e4151d0d124533338619f082c8c9a
cpath: /
copyroot: 0 /

_0.0.t1-1 add false false /A/B/E/bravo


904 1031
""")
  fp.close()

  exit_code, output, errput = svntest.main.run_svnadmin("verify", "-r2",
                                                        sbox.repo_dir)
  svntest.verify.verify_outputs(
    message=None, actual_stdout=output, actual_stderr=errput,
    expected_stdout=None,
    expected_stderr=".*Found malformed header in revision file")

#----------------------------------------------------------------------

def recover_fsfs(sbox):
  "recover a repository (FSFS only)"
  sbox.build()
  current_path = os.path.join(sbox.repo_dir, 'db', 'current')

  # Commit up to r3, so we can test various recovery scenarios.
  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newer line\n')
  svntest.main.run_svn(None, 'ci', sbox.wc_dir, '--quiet', '-m', 'log msg')

  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newest line\n')
  svntest.main.run_svn(None, 'ci', sbox.wc_dir, '--quiet', '-m', 'log msg')

  # Remember the contents of the db/current file.
  expected_current_contents = svntest.main.file_read(current_path)

  # Move aside the current file for r3.
  os.rename(os.path.join(sbox.repo_dir, 'db','current'),
            os.path.join(sbox.repo_dir, 'db','was_current'));

  # Run 'svnadmin recover' and check that the current file is recreated.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = svntest.main.file_read(current_path)
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

  # Now try writing db/current to be one rev lower than it should be.
  svntest.main.file_write(current_path, '2\n')

  # Run 'svnadmin recover' and check that the current file is fixed.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = svntest.main.file_read(current_path)
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

  # Now try writing db/current to be *two* revs lower than it should be.
  svntest.main.file_write(current_path, '1\n')

  # Run 'svnadmin recover' and check that the current file is fixed.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = svntest.main.file_read(current_path)
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

  # Now try writing db/current to be fish revs lower than it should be.
  #
  # Note: I'm not actually sure it's wise to recover from this, but
  # detecting it would require rewriting fs_fs.c:get_youngest() to
  # check the actual contents of its buffer, since atol() will happily
  # convert "fish" to 0.
  svntest.main.file_write(current_path, 'fish\n')

  # Run 'svnadmin recover' and check that the current file is fixed.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

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

  # Create 'sample' dir in sbox.repo_url, and load the dump stream there.
  svntest.actions.run_and_verify_svn(None,
                                     ['\n', 'Committed revision 1.\n'],
                                     [], "mkdir", sbox.repo_url + "/sample",
                                     "-m", "Create sample dir")
  load_and_verify_dumpstream(sbox, [], [], None, dumpfile, '--parent-dir',
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

  # Create 'sample-2' dir in sbox.repo_url, and load the dump stream again.
  # This time, don't include a leading slash on the --parent-dir argument.
  # See issue #3547.
  svntest.actions.run_and_verify_svn(None,
                                     ['\n', 'Committed revision 11.\n'],
                                     [], "mkdir", sbox.repo_url + "/sample-2",
                                     "-m", "Create sample-2 dir")
  load_and_verify_dumpstream(sbox, [], [], None, dumpfile, '--parent-dir',
                             'sample-2')

  # Verify the svn:mergeinfo properties for '--parent-dir'.
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample-2/branch - " +
                                      "/sample-2/trunk:15-17\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample-2/branch')
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample-2/branch1 - " +
                                      "/sample-2/branch:16-19\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample-2/branch1')

#----------------------------------------------------------------------

def set_uuid(sbox):
  "test 'svnadmin setuuid'"

  sbox.build(create_wc=False)

  # Squirrel away the original repository UUID.
  exit_code, output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  orig_uuid = output[0].rstrip()

  # Try setting a new, bogus UUID.
  svntest.actions.run_and_verify_svnadmin(None, None, '^.*Malformed UUID.*$',
                                          'setuuid', sbox.repo_dir, 'abcdef')

  # Try generating a brand new UUID.
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', sbox.repo_dir)
  exit_code, output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  new_uuid = output[0].rstrip()
  if new_uuid == orig_uuid:
    print("Error: new UUID matches the original one")
    raise svntest.Failure

  # Now, try setting the UUID back to the original value.
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', sbox.repo_dir, orig_uuid)
  exit_code, output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  new_uuid = output[0].rstrip()
  if new_uuid != orig_uuid:
    print("Error: new UUID doesn't match the original one")
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
  svntest.actions.run_and_verify_svn(None, ["/trunk:2-4\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/branch2')
  svntest.actions.run_and_verify_svn(None, ["/branch1:5-9\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/trunk')
  svntest.actions.run_and_verify_svn(None, ["/toplevel/trunk:11-13\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/toplevel/branch2')
  svntest.actions.run_and_verify_svn(None, ["/toplevel/branch1:14-18\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/toplevel/trunk')
  svntest.actions.run_and_verify_svn(None, ["/toplevel/trunk:11-12\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/toplevel/branch1')
  svntest.actions.run_and_verify_svn(None, ["/trunk:2-3\n"],
                                     [], 'propget', 'svn:mergeinfo',
                                     sbox.repo_url + '/branch1')

#----------------------------------------------------------------------

def fsfs_recover_handle_missing_revs_or_revprops_file(sbox):
  """fsfs recovery checks missing revs / revprops files"""
  # Set up a repository containing the greek tree.
  sbox.build()

  # Commit up to r3, so we can test various recovery scenarios.
  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newer line\n')
  svntest.main.run_svn(None, 'ci', sbox.wc_dir, '--quiet', '-m', 'log msg')

  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newest line\n')
  svntest.main.run_svn(None, 'ci', sbox.wc_dir, '--quiet', '-m', 'log msg')

  rev_3 = fsfs_file(sbox.repo_dir, 'revs', '3')
  rev_was_3 = rev_3 + '.was'

  # Move aside the revs file for r3.
  os.rename(rev_3, rev_was_3)

  # Verify 'svnadmin recover' fails when youngest has a revprops
  # file but no revs file.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.", None, errput, None,
    ".*Expected current rev to be <= %s but found 3"
    # For example, if svntest.main.fsfs_sharding == 2, then rev_3 would
    # be the pack file for r2:r3, and the error message would report "<= 1".
    % (rev_3.endswith('pack') and '[012]' or '2')):
    raise svntest.Failure

  # Restore the r3 revs file, thus repairing the repository.
  os.rename(rev_was_3, rev_3)

  revprop_3 = fsfs_file(sbox.repo_dir, 'revprops', '3')
  revprop_was_3 = revprop_3 + '.was'

  # Move aside the revprops file for r3.
  os.rename(revprop_3, revprop_was_3)

  # Verify 'svnadmin recover' fails when youngest has a revs file
  # but no revprops file (issue #2992).
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.", None, errput, None,
    ".*Revision 3 has a revs file but no revprops file"):
    raise svntest.Failure

  # Restore the r3 revprops file, thus repairing the repository.
  os.rename(revprop_was_3, revprop_3)

  # Change revprops file to a directory for revision 3
  os.rename(revprop_3, revprop_was_3)
  os.mkdir(revprop_3)

  # Verify 'svnadmin recover' fails when youngest has a revs file
  # but revprops file is not a file (another aspect of issue #2992).
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.", None, errput, None,
    ".*Revision 3 has a non-file where its revprops file should be.*"):
    raise svntest.Failure


#----------------------------------------------------------------------

def create_in_repo_subdir(sbox):
  "'svnadmin create /path/to/repo/subdir'"

  repo_dir = sbox.repo_dir
  wc_dir = sbox.wc_dir

  svntest.main.safe_rmtree(repo_dir, 1)
  svntest.main.safe_rmtree(wc_dir)

  # This should succeed
  svntest.main.create_repos(repo_dir)

  try:
    # This should fail
    subdir = os.path.join(repo_dir, 'Z')
    svntest.main.create_repos(subdir)
  except svntest.main.SVNRepositoryCreateFailure:
    return

  # No SVNRepositoryCreateFailure raised?
  raise svntest.Failure

def verify_with_invalid_revprops(sbox):
  "svnadmin verify detects invalid revprops file"

  repo_dir = sbox.repo_dir

  svntest.main.safe_rmtree(repo_dir, 1)

  # This should succeed
  svntest.main.create_repos(repo_dir)

  # Run a test verify
  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin verify' is unexpected.", None, errput, None,
    ".*Verified revision 0*"):
    raise svntest.Failure

  # Empty the revprops file
  rp_file = open(os.path.join(repo_dir, 'db', 'revprops', '0', '0'), 'w')

  rp_file.write('')
  rp_file.close()

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin verify' is unexpected.", None, errput, None,
    ".*Malformed file"):
    raise svntest.Failure

#----------------------------------------------------------------------
# More testing for issue #3020 'Reflect dropped/renumbered revisions in
# svn:mergeinfo data during svnadmin load'
#
# Specifically, test that loading a partial dump file filters out
# mergeinfo that refers to revisions that are older than the oldest
# loaded revisions -- See
# http://subversion.tigris.org/issues/show_bug.cgi?id=3020#desc10.
def drop_mergeinfo_outside_of_dump_stream(sbox):
  "filter mergeinfo revs outside of dump stream"

  test_create(sbox)

  # Load a partial dump into an existing repository.
  #
  # Picture == 1k words:
  #
  # The existing repos loaded from skeleton_repos.dump looks like this:
  #
  # Projects/       (Added r1)
  #   README        (Added r2)
  #   Project-X     (Added r3)
  #   Project-Y     (Added r4)
  #   Project-Z     (Added r5)
  #   docs/         (Added r6)
  #     README      (Added r6)
  #
  # The dump file 'mergeinfo_included_partial.dump' is a dump of r6:HEAD of
  # the following repos:
  #                       __________________________________________
  #                      |                                         |
  #                      |             ____________________________|_____
  #                      |            |                            |     |
  # trunk---r2---r3-----r5---r6-------r8---r9--------------->      |     |
  #   r1             |        |     |       |                      |     |
  # intial           |        |     |       |______                |     |
  # import         copy       |   copy             |            merge   merge
  #                  |        |     |            merge           (r5)   (r8)
  #                  |        |     |            (r9)              |     |
  #                  |        |     |              |               |     |
  #                  |        |     V              V               |     |
  #                  |        | branches/B2-------r11---r12---->   |     |
  #                  |        |     r7              |____|         |     |
  #                  |        |                        |           |     |
  #                  |      merge                      |___        |     |
  #                  |      (r6)                           |       |     |
  #                  |        |_________________           |       |     |
  #                  |                          |        merge     |     |
  #                  |                          |      (r11-12)    |     |
  #                  |                          |          |       |     |
  #                  V                          V          V       |     |
  #              branches/B1-------------------r10--------r13-->   |     |
  #                  r4                                            |     |
  #                   |                                            V     V
  #                  branches/B1/B/E------------------------------r14---r15->
  #                  
  #
  # The mergeinfo on the complete repos in the preceeding repos looks like:
  #
  #   Properties on 'branches/B1':
  #     svn:mergeinfo
  #       /branches/B2:11-12
  #       /trunk:6,9
  #   Properties on 'branches/B1/B/E':
  #     svn:mergeinfo
  #       /branches/B2/B/E:11-12
  #       /trunk/B/E:5-6,8-9
  #   Properties on 'branches/B2':
  #     svn:mergeinfo
  #       /trunk:9
  #
  # If we were to load the dump of r6:HEAD into an empty repository, we'd
  # expect any references to revisions <r6 to be removed entirely (since
  # that history no longer exists) and the the remaining mergeinfo should
  # have its revisions offset by -5.  The resulting mergeinfo should look
  # like this:
  #
  #   Properties on 'branches/B1':
  #     svn:mergeinfo
  #       /branches/B2:6-7
  #       /trunk:1,4
  #   Properties on 'branches/B1/B/E':
  #     svn:mergeinfo
  #       /branches/B2/B/E:6-7
  #       /trunk/B/E:1,3-4
  #   Properties on 'branches/B2':
  #     svn:mergeinfo
  #       /trunk:4
  #
  # But here we will load it into the existing skeleton repository in the
  # Projects/Project-X directory.  Since we are loading the dump into a
  # subtree, all the merge sources should be prefixed with the path to
  # that subtree, i.e. 'Projects/Project-X', compared to the mergeinfo above.
  # In addition, since the skeleton repos already has 6 revisions, we expect
  # all the remaining revisions to be offset +6 from the above.  That should
  # result in this mergeinfo:
  #
  #   Properties on 'Projects/Project-X/branches/B1':
  #     svn:mergeinfo
  #       /Projects/Project-X/branches/B2:12-13
  #       /Projects/Project-X/trunk:7,10
  #   Properties on 'Projects/Project-X/branches/B1/B/E':
  #     svn:mergeinfo
  #       /Projects/Project-X/branches/B2/B/E:12-13
  #       /Projects/Project-X/trunk/B/E:7,9-10
  #   Properties on 'Projects/Project-X/branches/B2':
  #     svn:mergeinfo
  #       /Projects/Project-X/trunk:10

  # Load the skeleton dump:
  dumpfile1 = svntest.main.file_read(
    os.path.join(os.path.dirname(sys.argv[0]),
                 'svnadmin_tests_data',
                 'skeleton_repos.dump'))
  load_and_verify_dumpstream(sbox, [], [], None, dumpfile1, '--ignore-uuid')

  # Load the partial repository with mergeinfo dump:
  dumpfile2 = svntest.main.file_read(
    os.path.join(os.path.dirname(sys.argv[0]),
                 'svnadmin_tests_data',
                 'mergeinfo_included_partial.dump'))
  load_and_verify_dumpstream(sbox, [], [], None, dumpfile2, '--ignore-uuid',
                             '--parent-dir', '/Projects/Project-X')

  # Check the resulting mergeinfo.
  #
  # TODO: Use pg -vR, which would make the expected output easier on the eyes.
  #       Not using it because pg -vR on windows is outputting <CR><CR><LF>
  #       after the first line of multiline mergeinfo, which breaks the
  #       comparison, e.g.:
  #
  #   Properties on 'Projects/Project-X/branches/B1/B/E':<CR><LF>
  #     svn:mergeinfo<CR><LF>
  #       /Projects/Project-X/branches/B2:12-13<CR><CR><LF>
  #                                            ^^^
  #       /Projects/Project-X/trunk:7,10<CR><LF>
  url = sbox.repo_url + '/Projects/Project-X/branches/'
  expected_output = svntest.verify.UnorderedOutput([
    url + "B1 - /Projects/Project-X/branches/B2:12-13\n",
    "/Projects/Project-X/trunk:7,10\n",
    url + "B2 - /Projects/Project-X/trunk:10\n",
    url + "B1/B/E - /Projects/Project-X/branches/B2/B/E:12-13\n",
    "/Projects/Project-X/trunk/B/E:7,9-10\n"])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)

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
              SkipUnless(verify_incremental_fsfs, svntest.main.is_fs_type_fsfs),
              SkipUnless(recover_fsfs, svntest.main.is_fs_type_fsfs),
              load_with_parent_dir,
              set_uuid,
              reflect_dropped_renumbered_revs,
              SkipUnless(fsfs_recover_handle_missing_revs_or_revprops_file,
                         svntest.main.is_fs_type_fsfs),
              create_in_repo_subdir,
              SkipUnless(verify_with_invalid_revprops,
                         svntest.main.is_fs_type_fsfs),
              drop_mergeinfo_outside_of_dump_stream,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

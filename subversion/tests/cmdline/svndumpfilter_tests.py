#!/usr/bin/env python
#
#  svndumpfilter_tests.py:  testing the 'svndumpfilter' tool.
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
import sys
import tempfile

# Our testing module
import svntest
from svntest.verify import SVNExpectedStdout, SVNExpectedStderr

# Get some helper routines from svnadmin_tests
from svnadmin_tests import load_and_verify_dumpstream, test_create

# (abbreviation)
Skip = svntest.testcase.Skip
SkipUnless = svntest.testcase.SkipUnless
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem


######################################################################
# Helper routines


def filter_and_return_output(dump, bufsize=0, *varargs):
  """Filter the array of lines passed in 'dump' and return the output"""

  if isinstance(dump, str):
    dump = [ dump ]

  ## TODO: Should we need to handle errput and exit_code?
  exit_code, output, errput = svntest.main.run_command_stdin(
    svntest.main.svndumpfilter_binary, None, bufsize, 1, dump, *varargs)

  return output


######################################################################
# Tests


def reflect_dropped_renumbered_revs(sbox):
  "reflect dropped renumbered revs in svn:mergeinfo"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2982. ##

  # Test svndumpfilter with include option
  test_create(sbox)
  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'with_merges.dump')
  dumpfile = open(dumpfile_location).read()

  filtered_out = filter_and_return_output(dumpfile, 0, "include",
                                          "trunk", "branch1",
                                          "--skip-missing-merge-sources",
                                          "--drop-empty-revs",
                                          "--renumber-revs", "--quiet")
  load_and_verify_dumpstream(sbox, [], [], None, filtered_out,
                             "--ignore-uuid")

  # Verify the svn:mergeinfo properties
  url = sbox.repo_url
  expected_output = svntest.verify.UnorderedOutput([
    url + "/trunk - /branch1:4-5\n",
    ])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)
  

  # Test svndumpfilter with exclude option
  test_create(sbox)
  filtered_out = filter_and_return_output(dumpfile, 0, "exclude",
                                          "branch1",
                                          "--skip-missing-merge-sources",
                                          "--drop-empty-revs",
                                          "--renumber-revs", "--quiet")
  load_and_verify_dumpstream(sbox, [], [], None, filtered_out,
                             "--ignore-uuid")

  # Verify the svn:mergeinfo properties
  expected_output = svntest.verify.UnorderedOutput([
    url + "/trunk - \n",
    ])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)

def svndumpfilter_loses_mergeinfo(sbox):
  "svndumpfilter loses mergeinfo"
  #svndumpfilter loses mergeinfo if invoked without --renumber-revs

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=3181. ##

  test_create(sbox)
  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'with_merges.dump')
  dumpfile = open(dumpfile_location).read()

  filtered_out = filter_and_return_output(dumpfile, 0, "include",
                                          "trunk", "branch1", "--quiet")
  load_and_verify_dumpstream(sbox, [], [], None, filtered_out)

  # Verify the svn:mergeinfo properties
  url = sbox.repo_url
  expected_output = svntest.verify.UnorderedOutput([
    url + "/trunk - /branch1:4-8\n",
    ])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)


def _simple_dumpfilter_test(sbox, dumpfile, *dumpargs):
  wc_dir = sbox.wc_dir

  filtered_output = filter_and_return_output(dumpfile, 0,
                                             '--quiet', *dumpargs)

  # Setup our expectations
  load_and_verify_dumpstream(sbox, [], [], None, filtered_output,
                             '--ignore-uuid')
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.remove('A/B/E/alpha')
  expected_disk.remove('A/B/E/beta')
  expected_disk.remove('A/B/E')
  expected_disk.remove('A/D/H/chi')
  expected_disk.remove('A/D/H/psi')
  expected_disk.remove('A/D/H/omega')
  expected_disk.remove('A/D/H')
  expected_disk.remove('A/D/G/pi')
  expected_disk.remove('A/D/G/rho')
  expected_disk.remove('A/D/G/tau')
  expected_disk.remove('A/D/G')

  expected_output = svntest.wc.State(wc_dir, {
    'A'           : Item(status='A '),
    'A/B'         : Item(status='A '),
    'A/B/lambda'  : Item(status='A '),
    'A/B/F'       : Item(status='A '),
    'A/mu'        : Item(status='A '),
    'A/C'         : Item(status='A '),
    'A/D'         : Item(status='A '),
    'A/D/gamma'   : Item(status='A '),
    'iota'        : Item(status='A '),
    })

  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.remove('A/B/E/alpha')
  expected_status.remove('A/B/E/beta')
  expected_status.remove('A/B/E')
  expected_status.remove('A/D/H/chi')
  expected_status.remove('A/D/H/psi')
  expected_status.remove('A/D/H/omega')
  expected_status.remove('A/D/H')
  expected_status.remove('A/D/G/pi')
  expected_status.remove('A/D/G/rho')
  expected_status.remove('A/D/G/tau')
  expected_status.remove('A/D/G')

  # Check that our paths really were excluded
  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status)


def dumpfilter_with_targets(sbox):
  "svndumpfilter --targets blah"
  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2697. ##

  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'greek_tree.dump')
  dumpfile = open(dumpfile_location).read()

  (fd, targets_file) = tempfile.mkstemp(dir=svntest.main.temp_dir)
  try:
    targets = open(targets_file, 'w')
    targets.write('/A/D/H\n')
    targets.write('/A/D/G\n')
    targets.close()
    _simple_dumpfilter_test(sbox, dumpfile,
                            'exclude', '/A/B/E', '--targets', targets_file)
  finally:
    os.close(fd)
    os.remove(targets_file)


def dumpfilter_with_patterns(sbox):
  "svndumpfilter --pattern PATH_PREFIX"

  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'greek_tree.dump')
  dumpfile = open(dumpfile_location).read()
  _simple_dumpfilter_test(sbox, dumpfile,
                          'exclude', '--pattern', '/A/D/[GH]*', '/A/[B]/E*')

#----------------------------------------------------------------------
# More testing for issue #3020 'Reflect dropped/renumbered revisions in
# svn:mergeinfo data during svnadmin load'
#
# Specifically, test that svndumpfilter, when used with the
# --skip-missing-merge-sources option, removes mergeinfo that refers to
# revisions that are older than the oldest revision in the dump stream.
def filter_mergeinfo_revs_outside_of_dump_stream(sbox):
  "filter mergeinfo revs outside of dump stream"

  test_create(sbox)

  # Load a partial dump into an existing repository.
  #
  # Picture == 1k words:
  #
  # The dump file we filter in this test, 'mergeinfo_included_partial.dump', is
  # a dump of r6:HEAD of the following repos:
  #
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
  # The mergeinfo on the complete repos would look like this:
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
  # We will run the partial dump through svndumpfilter using the the
  # --skip-missing-merge-soruces which should strip out any revisions < 6.
  # Then we'll load the filtered result into an empty repository.  This
  # should offset the incoming mergeinfo by -5.  The resulting mergeinfo
  # should look like this:
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
  partial_dump = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'mergeinfo_included_partial.dump')
  partial_dump_contents = open(partial_dump).read()
  filtered_dumpfile2 = filter_and_return_output(partial_dump_contents,
                                                8192, # Set a sufficiently
                                                      # large bufsize to
                                                      # avoid a deadlock
                                                "include", "trunk", "branches",
                                                "--skip-missing-merge-sources",
                                                "--quiet")
  load_and_verify_dumpstream(sbox, [], [], None, filtered_dumpfile2,
                             '--ignore-uuid')
  # Check the resulting mergeinfo.
  #
  # Currently this fails with this mergeinfo:
  #
  #   Properties on 'branches\B1':
  #     svn:mergeinfo
  #       /branches/B2:6-7
  #       /trunk:4,5-2
  #                ^^^
  # What's this?!?!  It's corrupt mergeinfo caused when svnadmin load
  # maps the end rev of the incoming 5-6, but not the start rev, see '2)'
  # in http://subversion.tigris.org/issues/show_bug.cgi?id=3020#desc16
  # svnadmin_tests.py 20 'don't filter mergeinfo revs from incremental dump'
  # also covers this problem.  Same problem here---
  #                                                |
  #   Properties on 'branches\B1\B\E':             |
  #     svn:mergeinfo                              |
  #       /branches/B2/B/E:6-7                     |
  #       /trunk/B/E:3-4,4-2 <---------------------
  #   Properties on 'branches\B2':
  #     svn:mergeinfo
  #       /trunk:4
  #
  # This test is set as XFail until --skip-missing-merge-sources gains its
  # new filtering functionality.
  url = sbox.repo_url + "/branches"
  expected_output = svntest.verify.UnorderedOutput([
    url + "/B1 - /branches/B2:6-7\n",
    "/trunk:1,4\n",
    url + "/B2 - /trunk:4\n",
    url + "/B1/B/E - /branches/B2/B/E:6-7\n",
    "/trunk/B/E:1,3-4\n"])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)
 
########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              reflect_dropped_renumbered_revs,
              svndumpfilter_loses_mergeinfo,
              dumpfilter_with_targets,
              dumpfilter_with_patterns,
              XFail(filter_mergeinfo_revs_outside_of_dump_stream),
              ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

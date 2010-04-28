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


########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              reflect_dropped_renumbered_revs,
              svndumpfilter_loses_mergeinfo,
              dumpfilter_with_targets,
              dumpfilter_with_patterns,
              ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

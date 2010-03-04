#!/usr/bin/env python
#
#  upgrade_tests.py:  test the working copy upgrade process
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

#
# These tests exercise the upgrade capabilities of 'svn upgrade' as it
# moves working copies between wc-1 and wc-ng.
#

import os
import re
import shutil
import sys
import tarfile
import tempfile

import svntest

Item = svntest.wc.StateItem
XFail = svntest.testcase.XFail
SkipUnless = svntest.testcase.SkipUnless

wc_is_too_old_regex = (".*Working copy format of '.*' is too old \(\d+\); " +
                    "please run 'svn upgrade'")


def get_current_format():
  # Get current format from subversion/libsvn_wc/wc.h
  format_file = open(os.path.join(os.path.dirname(__file__), "..", "..", "libsvn_wc", "wc.h")).read()
  return int(re.search("\n#define SVN_WC__VERSION (\d+)\n", format_file).group(1))


def replace_sbox_with_tarfile(sbox, tar_filename):
  try:
    svntest.main.safe_rmtree(sbox.wc_dir)
  except OSError, e:
    pass

  tarpath = os.path.join(os.path.dirname(sys.argv[0]), 'upgrade_tests_data',
                         tar_filename)
  t = tarfile.open(tarpath, 'r:bz2')
  extract_dir = tempfile.mkdtemp(dir=svntest.main.temp_dir)
  for member in t.getmembers():
    t.extract(member, extract_dir)

  shutil.move(os.path.join(extract_dir, tar_filename.split('.')[0]),
              sbox.wc_dir)


def check_format(sbox, expected_format):
  for root, dirs, files in os.walk(sbox.wc_dir):
    db = svntest.sqlite3.connect(os.path.join(root, '.svn', 'wc.db'))
    c = db.cursor()
    c.execute('pragma user_version;')
    found_format = c.fetchone()[0]
    db.close()

    if found_format != expected_format:
      raise svntest.Failure("found format '%d'; expected '%d'; in wc '%s'" %
                            (found_format, expected_format, root))

    if '.svn' in dirs:
      dirs.remove('.svn')


def check_dav_cache(dir_path, wc_id, expected_dav_caches):
  db = svntest.sqlite3.connect(os.path.join(dir_path, '.svn', 'wc.db'))
  c = db.cursor()

  for local_relpath, expected_dav_cache in expected_dav_caches.items():
    c.execute('select dav_cache from base_node ' +
              'where wc_id=? and local_relpath=?',
        (wc_id, local_relpath))
    dav_cache = str(c.fetchone()[0])

    if dav_cache != expected_dav_cache:
      raise svntest.Failure(
              "wrong dav cache for '%s'\n  Found:    '%s'\n  Expected: '%s'" %
                (dir_path, dav_cache, expected_dav_cache))

  db.close()


def run_and_verify_status_no_server(wc_dir, expected_status):
  "same as svntest.actions.run_and_verify_status(), but without '-u'"

  exit_code, output, errput = svntest.main.run_svn(None, 'st', '-q', '-v',
                                                   wc_dir)
  actual = svntest.tree.build_tree_from_status(output)
  try:
    svntest.tree.compare_trees("status", actual, expected_status.old_tree())
  except svntest.tree.SVNTreeError:
    svntest.verify.display_trees(None, 'STATUS OUTPUT TREE',
                                 output_tree, actual)
    print("ACTUAL STATUS TREE:")
    svvtest.tree.dump_tree_script(actual, wc_dir_name + os.sep)
    raise


def basic_upgrade(sbox):
  "basic upgrade behavior"

  replace_sbox_with_tarfile(sbox, 'basic_upgrade.tar.bz2')

  # Attempt to use the working copy, this should give an error
  expected_stderr = wc_is_too_old_regex
  svntest.actions.run_and_verify_svn(None, None, expected_stderr,
                                     'info', sbox.wc_dir)


  # Now upgrade the working copy
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'upgrade', sbox.wc_dir)

  # Actually check the format number of the upgraded working copy
  check_format(sbox, get_current_format())

  # Now check the contents of the working copy
  expected_status = svntest.actions.get_virginal_state(sbox.wc_dir, 1)
  run_and_verify_status_no_server(sbox.wc_dir, expected_status)


def upgrade_1_5_body(sbox, subcommand):
  replace_sbox_with_tarfile(sbox, 'upgrade_1_5.tar.bz2')

  # Attempt to use the working copy, this should give an error
  expected_stderr = wc_is_too_old_regex
  svntest.actions.run_and_verify_svn(None, None, expected_stderr,
                                     subcommand, sbox.wc_dir)


  # Now upgrade the working copy
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'upgrade', sbox.wc_dir)

  # Check the format of the working copy
  check_format(sbox, get_current_format())

  # Now check the contents of the working copy
  expected_status = svntest.actions.get_virginal_state(sbox.wc_dir, 1)
  run_and_verify_status_no_server(sbox.wc_dir, expected_status)


def upgrade_1_5(sbox):
  "test upgrading from a 1.5-era working copy"
  return upgrade_1_5_body(sbox, 'info')


def update_1_5(sbox):
  "test updating a 1.5-era working copy"

  # The 'update' printed:
  #    Skipped 'svn-test-work\working_copies\upgrade_tests-3'
  #    Summary of conflicts:
  #      Skipped paths: 1
  return upgrade_1_5_body(sbox, 'update')


def logs_left_1_5(sbox):
  "test upgrading from a 1.5-era wc with stale logs"

  replace_sbox_with_tarfile(sbox, 'logs_left_1_5.tar.bz2')

  # Try to upgrade, this should give an error
  expected_stderr = (".*Cannot upgrade with existing logs; please "
                     "run 'svn cleanup' with Subversion 1.6")
  svntest.actions.run_and_verify_svn(None, None, expected_stderr,
                                     'upgrade', sbox.wc_dir)


def upgrade_wcprops(sbox):
  "test upgrading a working copy with wcprops"

  replace_sbox_with_tarfile(sbox, 'upgrade_wcprops.tar.bz2')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'upgrade', sbox.wc_dir)

  # Make sure that .svn/all-wcprops has disappeared
  if os.path.exists(os.path.join(sbox.wc_dir, '.svn', 'all-wcprops')):
    raise svntest.Failure("all-wcprops file still exists")

  # Just for kicks, let's see if the wcprops are what we'd expect them
  # to be.  (This could be smarter.)
  expected_dav_caches = {
   '' :
    '(svn:wc:ra_dav:version-url 41 /svn-test-work/local_tmp/repos/!svn/ver/1)',
   'iota' :
    '(svn:wc:ra_dav:version-url 46 /svn-test-work/local_tmp/repos/!svn/ver/1/iota)',
  }
  check_dav_cache(sbox.wc_dir, 1, expected_dav_caches)
  
def basic_upgrade_1_0(sbox):
  "test upgrading a working copy created with 1.0.0"
  replace_sbox_with_tarfile(sbox, 'upgrade_1_0.tar.bz2')

  # Attempt to use the working copy, this should give an error
  expected_stderr = wc_is_too_old_regex
  svntest.actions.run_and_verify_svn(None, None, expected_stderr,
                                     'info', sbox.wc_dir)


  # Now upgrade the working copy
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'upgrade', sbox.wc_dir)

  # Actually check the format number of the upgraded working copy
  check_format(sbox, get_current_format())

  # Now check the contents of the working copy
  # #### This working copy is not just a basic tree,
  #      fix with the right data once we get here
  #expected_status = svntest.actions.get_virginal_state(sbox.wc_dir, 1)
  #run_and_verify_status_no_server(sbox.wc_dir, expected_status)


########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              basic_upgrade,
              upgrade_1_5,
              XFail(update_1_5),
              logs_left_1_5,
              upgrade_wcprops,
              XFail(basic_upgrade_1_0)
             ]


if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED

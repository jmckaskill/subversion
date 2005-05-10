#!/usr/bin/env python
#
#  stat_tests.py:  testing the svn stat command
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2004 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import string, sys, os.path, re, time

# Our testing module
import svntest


# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem



######################################################################
# Tests
#
#   Each test must return on success or raise on failure.

#----------------------------------------------------------------------

def status_unversioned_file_in_current_dir(sbox):
  "status on unversioned file in current directory"

  sbox.build()
  wc_dir = sbox.wc_dir

  was_cwd = os.getcwd()
  try:
    os.chdir(wc_dir)

    svntest.main.file_append('foo', 'a new file')

    svntest.actions.run_and_verify_svn(None, [ "?      foo\n" ], [],
                                       'stat', 'foo')

  finally:
    os.chdir(was_cwd)

#----------------------------------------------------------------------
# Regression for issue #590

def status_update_with_nested_adds(sbox):
  "run 'status -u' when nested additions are pending"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Make a backup copy of the working copy
  wc_backup = sbox.add_wc_path('backup')
  svntest.actions.duplicate_dir(wc_dir, wc_backup)
  
  # Create newdir and newfile
  newdir_path = os.path.join(wc_dir, 'newdir')
  newfile_path = os.path.join(wc_dir, 'newdir', 'newfile')
  os.makedirs(newdir_path)
  svntest.main.file_append (newfile_path, 'new text')

  # Schedule newdir and newfile for addition (note that the add is recursive)
  svntest.main.run_svn(None, 'add', newdir_path)

  # Created expected output tree for commit
  expected_output = svntest.wc.State(wc_dir, {
    'newdir' : Item(verb='Adding'),
    'newdir/newfile' : Item(verb='Adding'),
    })

  # Create expected status tree; all local revisions should be at 1,
  # but newdir and newfile should be at revision 2.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.add({
    'newdir' : Item(status='  ', wc_rev=2),
    'newdir/newfile' : Item(status='  ', wc_rev=2),
    })

  # Commit.
  svntest.actions.run_and_verify_commit (wc_dir, expected_output,
                                         expected_status, None,
                                         None, None, None, None, wc_dir)

  # Now we go to the backup working copy, still at revision 1.
  # We will run 'svn st -u', and make sure that newdir/newfile is reported
  # as a nonexistent (but pending) path.

  # Create expected status tree; all local revisions should be at 1,
  # but newdir and newfile should be present with 'blank' attributes.
  expected_status = svntest.actions.get_virginal_state(wc_backup, 2)
  expected_status.tweak(wc_rev=1)

  # Verify status.  Notice that we're running status *without* the
  # --quiet flag, so the unversioned items will appear.
  # Unfortunately, the regexp that we currently use to parse status
  # output is unable to parse a line that has no working revision!  If
  # an error happens, we'll catch it here.  So that's a good enough
  # regression test for now.  Someday, though, it would be nice to
  # positively match the mostly-empty lines.
  svntest.actions.run_and_verify_unquiet_status(wc_backup,
                                                expected_status)
  
#----------------------------------------------------------------------

# svn status -vN should include all entries in a directory
def status_shows_all_in_current_dir(sbox):
  "status -vN shows all items in current directory"

  sbox.build()
  wc_dir = sbox.wc_dir
  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  try:

    output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                     'stat', '-vN')

    if (len(output) != len(os.listdir("."))):
      raise svntest.Failure

  finally:
    os.chdir(was_cwd)


#----------------------------------------------------------------------

def status_missing_file(sbox):
  "status with a versioned file missing"

  sbox.build()
  wc_dir = sbox.wc_dir
  
  was_cwd = os.getcwd()
  
  os.chdir(wc_dir)
  try:

    os.remove('iota')

    output, err = svntest.actions.run_and_verify_svn(None, None, [], 'status')
    for line in output:
      if not re.match("! +iota", line):
        raise svntest.Failure

    # This invocation is for issue #2127.
    output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                     'status', '-u', 'iota')
    found_it = 0
    for line in output:
      if re.match("! +1 +iota", line):
        found_it = 1
    if not found_it:
      raise svntest.Failure

  finally:
    os.chdir(was_cwd)

#----------------------------------------------------------------------

def status_type_change(sbox):
  "status on versioned items whose type has changed"

  sbox.build()
  wc_dir = sbox.wc_dir

  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  try:

    # First replace a versioned dir with a file and a versioned file
    # with a versioned dir.
    os.rename('iota', 'was_iota')
    os.rename('A', 'iota')
    os.rename('was_iota', 'A')

    output, err = svntest.actions.run_and_verify_svn(None, None, [], 'status')
    if len(output) != 2:
      raise svntest.Failure
    for line in output:
      if not re.match("~ +(iota|A)", line):
        raise svntest.Failure

    # Now change the file that is obstructing the versioned dir into an
    # unversioned dir.
    os.remove('A')
    os.mkdir('A')

    output, err = svntest.actions.run_and_verify_svn(None, None, [], 'status')
    if len(output) != 2:
      raise svntest.Failure
    for line in output:
      if not re.match("~ +(iota|A)", line):
        raise svntest.Failure

    # Now change the versioned dir that is obstructing the file into an
    # unversioned dir.
    svntest.main.safe_rmtree('iota')
    os.mkdir('iota')

    output, err = svntest.actions.run_and_verify_svn(None, None, [], 'status')
    if len(output) != 2:
      raise svntest.Failure
    for line in output:
      if not re.match("~ +(iota|A)", line):
        raise svntest.Failure

  finally:
    os.chdir(was_cwd)

#----------------------------------------------------------------------

def status_type_change_to_symlink(sbox):
  "status on versioned items replaced by symlinks"

  sbox.build()
  wc_dir = sbox.wc_dir
  
  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  try:

    # "broken" symlinks
    os.remove('iota')
    os.symlink('foo', 'iota')
    svntest.main.safe_rmtree('A/D')
    os.symlink('bar', 'A/D')

    output, err = svntest.actions.run_and_verify_svn(None, None, [], 'status')
    if len(output) != 2:
      raise svntest.Failure
    for line in output:
      if not re.match("~ +(iota|A/D)", line):
        raise svntest.Failure

    # "valid" symlinks
    os.remove('iota')
    os.remove('A/D')
    os.symlink('A/mu', 'iota')
    os.symlink('C', 'A/D')

    output, err = svntest.actions.run_and_verify_svn(None, None, [], 'status')
    if len(output) != 2:
      raise svntest.Failure
    for line in output:
      if not re.match("~ +(iota|A/D)", line):
        raise svntest.Failure

  finally:
    os.chdir(was_cwd)

#----------------------------------------------------------------------
# Regression test for revision 3686.

def status_with_new_files_pending(sbox):
  "status -u with new files in the repository"

  sbox.build()
  wc_dir = sbox.wc_dir
  
  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  try:
    svntest.main.file_append('newfile', 'this is a new file')
    svntest.main.run_svn(None, 'add', 'newfile')
    svntest.main.run_svn(None, 'ci', '-m', 'logmsg')
    svntest.main.run_svn(None, 'up', '-r', '1')

    output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                     'status', '-u')

    # The bug fixed in revision 3686 was a seg fault.  We don't have a
    # reliable way to detect a seg fault here, since we haven't dealt
    # with the popen2{Popen3,Popen4} mess in Python yet (the latter two
    # are classes within the first, which is a module, and the Popen3
    # class is not the same as os.popen3().  Got that?)  See the Python
    # docs for details; in the meantime, no output means there was a
    # problem.
    for line in output:
      if line.find('newfile') != -1:
        break
    else:
      raise svntest.Failure

  finally:
    os.chdir(was_cwd)

#----------------------------------------------------------------------

def status_for_unignored_file(sbox):
  "status for unignored file and directory"

  sbox.build()
  wc_dir = sbox.wc_dir
  
  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  try:
    svntest.main.file_append('newfile', 'this is a new file')
    os.makedirs('newdir')
    svntest.main.run_svn(None, 'propset', 'svn:ignore', 'new*', '.')

    # status on the directory with --no-ignore
    svntest.actions.run_and_verify_svn(None,
                                       ['I      newdir\n',
                                        'I      newfile\n',
                                        ' M     .\n'],
                                       [],
                                       'status', '--no-ignore', '.')

    # status specifying the file explicitly on the command line
    svntest.actions.run_and_verify_svn(None,
                                       ['I      newdir\n',
                                        'I      newfile\n'],
                                       [],
                                       'status', 'newdir', 'newfile')
  
  finally:
    os.chdir(was_cwd)


#----------------------------------------------------------------------

def status_for_nonexistent_file(sbox):
  "status on missing and unversioned file"

  sbox.build()

  wc_dir = sbox.wc_dir
  was_cwd = os.getcwd()

  os.chdir(wc_dir)

  try:
    output, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                     'status',
                                                     'nonexistent-file')

    # there should *not* be a status line printed for the nonexistent file 
    for line in output:
      if re.match(" +nonexistent-file", line):
        raise svntest.Failure
  
  finally:
    os.chdir(was_cwd)


#----------------------------------------------------------------------

def status_file_needs_update(sbox):
  "status -u indicates out-of-dateness"

  # See this thread:
  #
  #    http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgNo=27975
  #
  # Basically, Andreas was seeing inconsistent results depending on
  # whether or not he accompanied 'svn status -u' with '-v':
  #
  #    % svn st -u
  #    Head revision:     67
  #    %
  #
  # ...and yet...
  # 
  #    % svn st -u -v
  #                   56        6          k   cron-daily.pl
  #           *       56       44          k   crontab.root
  #                   56        6          k   gmls-lR.pl
  #    Head revision:     67
  #    %
  #
  # The first status should show the asterisk, too.  There was never
  # any issue for this bug, so this comment and the thread are your
  # audit trail :-).

  sbox.build()
  wc_dir = sbox.wc_dir
  
  other_wc = sbox.add_wc_path('other')

  svntest.actions.duplicate_dir(wc_dir, other_wc)

  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  svntest.main.file_append('crontab.root', 'New file crontab.root.\n')
  svntest.main.run_svn(None, 'add', 'crontab.root')
  svntest.main.run_svn(None, 'ci', '-m', 'log msg')
  os.chdir(was_cwd)
  os.chdir(other_wc)
  svntest.main.run_svn(None, 'up')

  os.chdir(was_cwd)
  os.chdir(wc_dir)
  svntest.main.file_append('crontab.root', 'New line in crontab.root.\n')
  svntest.main.run_svn(None, 'ci', '-m', 'log msg')

  # The `svntest.actions.run_and_verify_*_status' routines all pass
  # the -v flag, which we don't want, as this bug never appeared when
  # -v was passed.  So we run status by hand:
  os.chdir(was_cwd)
  out, err = svntest.actions.run_and_verify_svn(None, None, [],
                                                'status', '-u', other_wc)

  for line in out:
    if re.match("\\s+\\*.*crontab\\.root$", line):
      break
  else:
    raise svntest.Failure

#----------------------------------------------------------------------

def status_uninvited_parent_directory(sbox):
  "status -u on outdated, added file shows only that"

  # To reproduce, check out working copies wc1 and wc2, then do:
  #
  #   $ cd wc1
  #   $ echo "new file" >> newfile
  #   $ svn add newfile
  #   $ svn ci -m 'log msg'
  #
  #   $ cd ../wc2
  #   $ echo "new file" >> newfile
  #   $ svn add newfile
  #
  #   $ cd ..
  #   $ svn st wc2/newfile
  #
  # You *should* get one line of status output, for newfile.  The bug
  # is that you get two instead, one for newfile, and one for its
  # parent directory, wc2/.
  #
  # This bug was originally discovered during investigations into
  # issue #1042, "fixed" in revision 4181, then later the fix was
  # reverted because it caused other status problems (see the test
  # status_file_needs_update(), which fails when 4181 is present).

  sbox.build()
  wc_dir = sbox.wc_dir
  
  other_wc = sbox.add_wc_path('other')

  svntest.actions.duplicate_dir(wc_dir, other_wc)

  was_cwd = os.getcwd()

  os.chdir(wc_dir)
  svntest.main.file_append('newfile', 'New file.\n')
  svntest.main.run_svn(None, 'add', 'newfile')
  svntest.main.run_svn(None, 'ci', '-m', 'log msg')

  os.chdir(was_cwd)
  os.chdir(other_wc)
  svntest.main.file_append('newfile', 'New file.\n')
  svntest.main.run_svn(None, 'add', 'newfile')

  os.chdir(was_cwd)

  # We don't want a full status tree here, just one line (or two, if
  # the bug is present).  So run status by hand:
  os.chdir(was_cwd)
  out, err = svntest.actions.run_and_verify_svn(
    None, None, [],
    'status', '-u', os.path.join(other_wc, 'newfile'))

  for line in out:
    # The "/?" is just to allow for an optional trailing slash.
    if re.match("\\s+\\*.*\.other/?$", line):
      raise svntest.Failure

def status_on_forward_deletion(sbox):
  "status -u on working copy deleted in HEAD"
  # See issue #1289.
  sbox.build()
  wc_dir = sbox.wc_dir
  
  top_url = svntest.main.current_repo_url
  A_url = top_url + '/A'

  svntest.main.run_svn(None, 'rm', '-m', 'Remove A.', A_url)

  svntest.main.safe_rmtree(wc_dir)
  os.mkdir(wc_dir)
  saved_cwd = os.getcwd()
  os.chdir(wc_dir)
  try:
    svntest.main.run_svn(None, 'co', '-r1', top_url + "@1", 'wc')
    # If the bug is present, this will error with
    #
    #    subversion/libsvn_wc/lock.c:513: (apr_err=155005)
    #    svn: Working copy not locked
    #    svn: directory '' not locked
    #
    svntest.actions.run_and_verify_svn(None, None, [], 'st', '-u', 'wc')

    # Try again another way; the error would look like this:
    #
    #    subversion/libsvn_repos/delta.c:207: (apr_err=160005)
    #    svn: Invalid filesystem path syntax
    #    svn: svn_repos_dir_delta: invalid editor anchoring; at least \
    #       one of the input paths is not a directory and there was   \
    #       no source entry.
    #
    # (Dang!  Hope a user never has to see that :-) ).
    #
    svntest.main.safe_rmtree('wc')
    svntest.main.run_svn(None, 'co', '-r1', A_url + "@1", 'wc')
    svntest.actions.run_and_verify_svn(None, None, [], 'st', '-u', 'wc')
    
  finally:
    os.chdir(saved_cwd)

#----------------------------------------------------------------------

def get_last_changed_date(path):
  "get the Last Changed Date for path using svn info"
  out, err = svntest.actions.run_and_verify_svn(None, None, [], 'info', path)
  for line in out:
    if re.match("^Last Changed Date", line):
      return line
  print "Didn't find Last Changed Date for " + path
  raise svntest.Failure

# Helper for timestamp_behaviour test
def get_prop_timestamp(path):
  "get the prop-time for path using svn info"
  out, err = svntest.actions.run_and_verify_svn(None, None, [], 'info', path)
  for line in out:
    if re.match("^Properties Last Updated", line):
      return line
  print "Didn't find prop-time for " + path
  raise svntest.Failure

# Helper for timestamp_behaviour test
def get_text_timestamp(path):
  "get the text-time for path using svn info"
  out, err = svntest.actions.run_and_verify_svn(None, None, [], 'info', path)
  for line in out:
    if re.match("^Text Last Updated", line):
      return line
  print "Didn't find text-time for " + path
  raise svntest.Failure

# Helper for timestamp_behaviour test
def prop_time_behaviour(wc_dir, wc_path, status_path, expected_status, cmd):
  "prop-time behaviour"

  # Pristine prop-time
  pre_prop_time = get_prop_timestamp(wc_path)

  # Modifying the property does not affect the prop-time
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'name', 'xxx', wc_path)
  expected_status.tweak(status_path, status=' M')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  prop_time = get_prop_timestamp(wc_path)
  if prop_time != pre_prop_time:
    raise svntest.Failure

  # Manually reverting the property does not affect the prop-time
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'name', 'value', wc_path)
  expected_status.tweak(status_path, status='  ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  prop_time = get_prop_timestamp(wc_path)
  if prop_time != pre_prop_time:
    raise svntest.Failure

  # revert/cleanup change the prop-time even though the properties don't change
  if cmd == 'cleanup':
    svntest.actions.run_and_verify_svn(None, None, [], cmd, wc_dir)
  else:
    svntest.actions.run_and_verify_svn(None, None, [], cmd, wc_path)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  prop_time = get_prop_timestamp(wc_path)
  if prop_time == pre_prop_time:
    raise svntest.Failure

# Helper for timestamp_behaviour test
def text_time_behaviour(wc_dir, wc_path, status_path, expected_status, cmd):
  "text-time behaviour"

  # Pristine text and text-time
  fp = open(wc_path, 'r')
  pre_text = fp.readlines()
  pre_text_time = get_text_timestamp(wc_path)

  # Modifying the text does not affect text-time
  svntest.main.file_append (wc_path, "some mod")
  expected_status.tweak(status_path, status='M ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  text_time = get_text_timestamp(wc_path)
  if text_time != pre_text_time:
    raise svntest.Failure

  # Manually reverting the text does not affect the text-time
  fp = open(wc_path, 'w')
  fp.writelines(pre_text)
  fp.close()
  expected_status.tweak(status_path, status='  ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  text_time = get_text_timestamp(wc_path)
  if text_time != pre_text_time:
    raise svntest.Failure

  # revert/cleanup change the text-time even though the text doesn't change
  if cmd == 'cleanup':
    svntest.actions.run_and_verify_svn(None, None, [], cmd, wc_dir)
  else:
    svntest.actions.run_and_verify_svn(None, None, [], cmd, wc_path)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  text_time = get_text_timestamp(wc_path)
  if text_time == pre_text_time:
    raise svntest.Failure


# Is this really a status test?  I'm not sure, but I don't know where
# else to put it.
def timestamp_behaviour(sbox):
  "timestamp behaviour"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Setup a file and directory with properties
  A_path = os.path.join(wc_dir, 'A')
  iota_path = os.path.join(wc_dir, 'iota')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'propset', 'name', 'value',
                                     A_path, iota_path)
  svntest.actions.run_and_verify_svn(None, None, [], 'commit',
                                     '--username', svntest.main.wc_author,
                                     '--password', svntest.main.wc_passwd,
                                     '--message', 'log message',
                                     wc_dir)

  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak(wc_rev=1)
  expected_status.tweak('iota', 'A', wc_rev=2)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # Sleep to ensure timestamps change
  time.sleep(2)

  # Check behaviour of revert on prop-time
  prop_time_behaviour(wc_dir, iota_path, 'iota', expected_status, 'revert')
  prop_time_behaviour(wc_dir, A_path, 'A', expected_status, 'revert')
  # Check behaviour of revert on text-time
  text_time_behaviour(wc_dir, iota_path, 'iota', expected_status, 'revert')

  # Sleep to ensure timestamps change
  time.sleep(2)

  # Check behaviour of cleanup on prop-time
  prop_time_behaviour(wc_dir, iota_path, 'iota', expected_status, 'cleanup')
  prop_time_behaviour(wc_dir, A_path, 'A', expected_status, 'cleanup')
  # Check behaviour of cleanup on text-time
  text_time_behaviour(wc_dir, iota_path, 'iota', expected_status, 'cleanup')

  # Create a config to enable use-commit-times
  config_dir = os.path.join(os.path.abspath(svntest.main.temp_dir),
                            'use_commit_config')
  if not os.path.isdir(config_dir):
    os.makedirs(config_dir)
  fd = open(os.path.join(config_dir, 'config'), 'w')
  fd.write('[miscellany]\n')
  fd.write('use-commit-times = yes\n')
  fd.close()
  fd = open(os.path.join(config_dir, 'server'), 'w')
  fd.write('\n')
  fd.close()
  svntest.main.set_config_dir(config_dir)

  other_wc = sbox.add_wc_path('other')
  svntest.actions.run_and_verify_svn("checkout failed", None, [],
                                     'co', svntest.main.current_repo_url,
                                     '--username',
                                     svntest.main.wc_author,
                                     '--password',
                                     svntest.main.wc_passwd,
                                     other_wc)

  other_iota_path = os.path.join(other_wc, 'iota')
  iota_text_timestamp = get_text_timestamp(other_iota_path)
  iota_last_changed = get_last_changed_date(other_iota_path)
  if (iota_text_timestamp[17] != ':' or
      iota_text_timestamp[17:] != iota_last_changed[17:]):
    raise svntest.Failure

  ### FIXME: check the working file's timestamp as well

#----------------------------------------------------------------------

def status_on_unversioned_dotdot(sbox):
  "status on '..' where '..' is not versioned"
  # See issue #1617.
  sbox.build()
  wc_dir = sbox.wc_dir
  
  new_dir = os.path.join(wc_dir, 'new_dir')
  new_subdir = os.path.join(new_dir, 'new_subdir')
  os.mkdir(new_dir)
  os.mkdir(new_subdir)
  
  saved_cwd = os.getcwd()
  os.chdir(new_subdir)
  try:
    out, err = svntest.main.run_svn(1, 'st', '..')
    for line in err:
      if line.find('svn: \'..\' is not a working copy') != -1:
        break
    else:
      raise svntest.Failure
  finally:
    os.chdir(saved_cwd)

#----------------------------------------------------------------------

def status_on_partially_nonrecursive_wc(sbox):
  "status -u in partially non-recursive wc"
  # Based on issue #2122.
  #
  #    $ svn co -N -r 213 svn://svn.debian.org/pkg-kde .
  #    A  README
  #    Checked out revision 213.
  #    
  #    $ svn up -r 213 scripts www
  #    [ List of scripts/* files.]
  #    Updated to revision 213.
  #    [ List of www/* files.]
  #    Updated to revision 213.
  #    
  #    $ svn st -u
  #       *      213   www/IGNORE-ME
  #       *      213   www
  #    svn: subversion/libsvn_wc/status.c:910: tweak_statushash:         \
  #         Assertion `repos_text_status == svn_wc_status_added' failed. \
  #         Aborted (core dumped)
  #
  # You might think that the intermediate "svn up -r 213 scripts www"
  # step is unnecessary, but when I tried eliminating it, I got
  #
  #    $ svn st -u
  #    subversion/libsvn_wc/lock.c:642: (apr_err=155005)
  #    svn: Working copy 'www' not locked
  #    $ 
  #
  # instead of the assertion error.

  sbox.build()
  wc_dir = sbox.wc_dir
  
  top_url = svntest.main.current_repo_url
  A_url = top_url + '/A'
  D_url = top_url + '/A/D'
  G_url = top_url + '/A/D/G'
  H_url = top_url + '/A/D/H'
  rho = os.path.join(wc_dir, 'A', 'D', 'G', 'rho')

  # Commit a change to A/D/G/rho.  This will be our equivalent of
  # whatever change it was that happened between r213 and HEAD in the
  # reproduction recipe.  For us, it's r2.
  svntest.main.file_append(rho, 'Whan that Aprille with his shoores soote\n')
  svntest.main.run_svn(None, 'ci', '-m', 'log msg', rho)

  # Make the working copy weird in the right way, then try status -u.
  D_wc = sbox.add_wc_path('D')
  svntest.main.run_svn(None, 'co', '-r1', '-N', D_url, D_wc)
  saved_cwd = os.getcwd()
  try:
    os.chdir(D_wc)
    svntest.main.run_svn(None, 'up', '-r1', 'H')
    svntest.main.run_svn(None, 'st', '-u')
  finally:
    os.chdir(saved_cwd)


def missing_dir_in_anchor(sbox):
  "a missing dir in the anchor"

  sbox.build()
  wc_dir = sbox.wc_dir

  foo_path = os.path.join(wc_dir, 'foo')
  svntest.actions.run_and_verify_svn(None, None, [], 'mkdir', foo_path)
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.add({
    'foo' : Item(status='A ', wc_rev=0),
    })
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # At one point this caused a "foo not locked" error
  svntest.main.safe_rmtree(foo_path)
  expected_status.tweak('foo', status='! ', wc_rev='?')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)


#----------------------------------------------------------------------  


########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              status_unversioned_file_in_current_dir,
              status_update_with_nested_adds,
              status_shows_all_in_current_dir,
              status_missing_file,
              status_type_change,
              Skip(status_type_change_to_symlink, (os.name != 'posix')),
              status_with_new_files_pending,
              status_for_unignored_file,
              status_for_nonexistent_file,
              status_file_needs_update,
              status_uninvited_parent_directory,
              status_on_forward_deletion,
              timestamp_behaviour,
              status_on_unversioned_dotdot,
              status_on_partially_nonrecursive_wc,
              missing_dir_in_anchor,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

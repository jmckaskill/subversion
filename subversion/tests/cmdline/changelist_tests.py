#!/usr/bin/env python
#
#  changelist_tests.py:  testing changelist uses.
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2008 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import string, sys, os, re

# Our testing module
import svntest

# (abbreviation)
Skip = svntest.testcase.Skip
SkipUnless = svntest.testcase.SkipUnless
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem


######################################################################
# Utilities


def mod_all_files(wc_dir, new_text):
  """Walk over working copy WC_DIR, appending NEW_TEXT to all the
  files in that tree (but not inside the .svn areas of that tree)."""
  
  def tweak_files(new_text, dirname, names):
    if os.path.basename(dirname) == ".svn":
      del names[:]
    else:
      for name in names:
        full_path = os.path.join(dirname, name)
        if os.path.isfile(full_path):
          svntest.main.file_append(full_path, new_text)
        
  os.path.walk(wc_dir, tweak_files, new_text)

def changelist_all_files(wc_dir, name_func):
  """Walk over working copy WC_DIR, adding versioned files to
  changelists named by invoking NAME_FUNC(full-path-of-file) and
  noting its string return value (or None, if we wish to remove the
  file from a changelist)."""
  
  def do_changelist(name_func, dirname, names):
    if os.path.basename(dirname) == ".svn":
      del names[:]
    else:
      for name in names:
        full_path = os.path.join(dirname, name)
        if os.path.isfile(full_path):
          clname = name_func(full_path)
          if not clname:
            svntest.main.run_svn(None, "changelist", "--remove", full_path)
          else:
            svntest.main.run_svn(None, "changelist", clname, full_path)
        
  os.path.walk(wc_dir, do_changelist, name_func)

def clname_from_lastchar_cb(full_path):
  """Callback for changelist_all_files() that returns a changelist
  name matching the last character in the file's name.  For example,
  after running this on a greek tree where every file has some text
  modification, 'svn status' shows:
  
    --- Changelist 'a':
    M      A/B/lambda
    M      A/B/E/alpha
    M      A/B/E/beta
    M      A/D/gamma
    M      A/D/H/omega
    M      iota
    
    --- Changelist 'u':
    M      A/mu
    M      A/D/G/tau
    
    --- Changelist 'i':
    M      A/D/G/pi
    M      A/D/H/chi
    M      A/D/H/psi
    
    --- Changelist 'o':
    M      A/D/G/rho
    """
  return full_path[-1]


# Regular expressions for 'svn changelist' output.
_re_cl_add  = re.compile("Path '(.*)' is now a member of changelist '(.*)'.")
_re_cl_rem  = re.compile("Path '(.*)' is no longer a member of a changelist.")

def verify_changelist_output(output, expected_adds=None,
                             expected_removals=None):
  """Compare lines of OUTPUT from 'svn changelist' against
  EXPECTED_ADDS (a dictionary mapping paths to changelist names) and
  EXPECTED_REMOVALS (a dictionary mapping paths to ... whatever)."""

  num_expected = 0
  if expected_adds:
    num_expected += len(expected_adds)
  if expected_removals:
    num_expected += len(expected_removals)
    
  if len(output) != num_expected:
    raise svntest.Failure("Unexpected number of 'svn changelist' output lines")

  for line in output:
    line = line.rstrip()
    match = _re_cl_rem.match(line)
    if match \
       and expected_removals \
       and expected_removals.has_key(match.group(1)):
        continue
    elif match:
      raise svntest.Failure("Unexpected changelist removal line: " + line)    
    match = _re_cl_add.match(line)
    if match \
       and expected_adds \
       and expected_adds.get(match.group(1)) == match.group(2):
        continue
    elif match:
      raise svntest.Failure("Unexpected changelist add line: " + line)    
    raise svntest.Failure("Unexpected line: " + line)

def verify_pget_output(output, expected_props):
  """Compare lines of OUTPUT from 'svn propget' against EXPECTED_PROPS
  (a dictionary mapping paths to property values)."""
  
  _re_pget = re.compile('^(.*) - (.*)$')
  actual_props = {}
  for line in output:
    try:
      path, prop = line.rstrip().split(' - ')
    except:
      raise svntest.Failure("Unexpected output line: " + line)
    actual_props[path] = prop
  if expected_props != actual_props:
    raise svntest.Failure("Got unexpected property results")
  
    
######################################################################
# Tests
#
#   Each test must return on success or raise on failure.


#----------------------------------------------------------------------

def add_remove_changelists(sbox):
  "add and remove files from changelists"

  sbox.build()
  wc_dir = sbox.wc_dir

  ### First, we play with just adding to changelists ###
  
  # svn changelist foo WC_DIR
  output, errput = svntest.main.run_svn(None, "changelist", "foo",
                                        wc_dir)
  verify_changelist_output(output) # nothing expected

  # svn changelist foo WC_DIR --depth files
  output, errput = svntest.main.run_svn(None, "changelist", "foo",
                                        "--depth", "files",
                                        wc_dir)
  expected_adds = {
    os.path.join(wc_dir, 'iota') : 'foo',
    }
  verify_changelist_output(output, expected_adds)
  
  # svn changelist foo WC_DIR --depth infinity
  output, errput = svntest.main.run_svn(None, "changelist", "foo",
                                        "--depth", "infinity",
                                        wc_dir)
  expected_adds = {
    os.path.join(wc_dir, 'A', 'B', 'E', 'alpha') : 'foo',
    os.path.join(wc_dir, 'A', 'B', 'E', 'beta') : 'foo',
    os.path.join(wc_dir, 'A', 'B', 'lambda') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'G', 'pi') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'G', 'rho') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'G', 'tau') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'H', 'chi') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'H', 'psi') : 'foo',
    os.path.join(wc_dir, 'A', 'D', 'gamma') : 'foo',
    os.path.join(wc_dir, 'A', 'mu') : 'foo',
    }
  verify_changelist_output(output, expected_adds)

  ### Now, change some changelists ###
  
  # svn changelist bar WC_DIR/A/D --depth infinity
  output, errput = svntest.main.run_svn(".*", "changelist", "bar",
                                        "--depth", "infinity",
                                        os.path.join(wc_dir, 'A', 'D'))
  expected_adds = {
    os.path.join(wc_dir, 'A', 'D', 'G', 'pi') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'G', 'rho') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'G', 'tau') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'H', 'chi') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'H', 'psi') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'gamma') : 'bar',
    }
  verify_changelist_output(output, expected_adds)

  # svn changelist baz WC_DIR/A/D/H --depth infinity
  output, errput = svntest.main.run_svn(".*", "changelist", "baz",
                                        "--depth", "infinity",
                                        os.path.join(wc_dir, 'A', 'D', 'H'))
  expected_adds = {
    os.path.join(wc_dir, 'A', 'D', 'H', 'chi') : 'baz',
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : 'baz',
    os.path.join(wc_dir, 'A', 'D', 'H', 'psi') : 'baz',
    }
  verify_changelist_output(output, expected_adds)

  ### Now, let's selectively rename some changelists ###

  # svn changelist foo-rename WC_DIR --depth infinity --changelist foo
  output, errput = svntest.main.run_svn(".*", "changelist", "foo-rename",
                                        "--depth", "infinity",
                                        "--changelist", "foo",
                                        wc_dir)
  expected_adds = {
    os.path.join(wc_dir, 'A', 'B', 'E', 'alpha') : 'foo-rename',
    os.path.join(wc_dir, 'A', 'B', 'E', 'beta') : 'foo-rename',
    os.path.join(wc_dir, 'A', 'B', 'lambda') : 'foo-rename',
    os.path.join(wc_dir, 'A', 'mu') : 'foo-rename',
    os.path.join(wc_dir, 'iota') : 'foo-rename',
    }
  verify_changelist_output(output, expected_adds)

  # svn changelist bar WC_DIR --depth infinity
  #     --changelist foo-rename --changelist baz
  output, errput = svntest.main.run_svn(".*", "changelist", "bar",
                                        "--depth", "infinity",
                                        "--changelist", "foo-rename",
                                        "--changelist", "baz",
                                        wc_dir)
  expected_adds = {
    os.path.join(wc_dir, 'A', 'B', 'E', 'alpha') : 'bar',
    os.path.join(wc_dir, 'A', 'B', 'E', 'beta') : 'bar',
    os.path.join(wc_dir, 'A', 'B', 'lambda') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'H', 'chi') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : 'bar',
    os.path.join(wc_dir, 'A', 'D', 'H', 'psi') : 'bar',
    os.path.join(wc_dir, 'A', 'mu') : 'bar',
    os.path.join(wc_dir, 'iota') : 'bar',
    }
  verify_changelist_output(output, expected_adds)

  ### Okay.  Time to remove some stuff from changelists now. ###
  
  # svn changelist --remove WC_DIR
  output, errput = svntest.main.run_svn(None, "changelist", "--remove",
                                        wc_dir)
  verify_changelist_output(output) # nothing expected

  # svn changelist --remove WC_DIR --depth files
  output, errput = svntest.main.run_svn(None, "changelist", "--remove",
                                        "--depth", "files",
                                        wc_dir)
  expected_removals = {
    os.path.join(wc_dir, 'iota') : None,
    }
  verify_changelist_output(output, None, expected_removals)
  
  # svn changelist --remove WC_DIR --depth infinity
  output, errput = svntest.main.run_svn(None, "changelist", "--remove",
                                        "--depth", "infinity",
                                        wc_dir)
  expected_removals = {
    os.path.join(wc_dir, 'A', 'B', 'E', 'alpha') : None,
    os.path.join(wc_dir, 'A', 'B', 'E', 'beta') : None,
    os.path.join(wc_dir, 'A', 'B', 'lambda') : None,
    os.path.join(wc_dir, 'A', 'D', 'G', 'pi') : None,
    os.path.join(wc_dir, 'A', 'D', 'G', 'rho') : None,
    os.path.join(wc_dir, 'A', 'D', 'G', 'tau') : None,
    os.path.join(wc_dir, 'A', 'D', 'H', 'chi') : None,
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : None,
    os.path.join(wc_dir, 'A', 'D', 'H', 'psi') : None,
    os.path.join(wc_dir, 'A', 'D', 'gamma') : None,
    os.path.join(wc_dir, 'A', 'mu') : None,
    }
  verify_changelist_output(output, None, expected_removals)
  
  ### Add files to changelists based on the last character in their names ###
  
  changelist_all_files(wc_dir, clname_from_lastchar_cb)

  ### Now, do selective changelist removal ###
  
  # svn changelist --remove WC_DIR --depth infinity --changelist a
  output, errput = svntest.main.run_svn(None, "changelist", "--remove",
                                        "--depth", "infinity",
                                        "--changelist", "a",
                                        wc_dir)
  expected_removals = {
    os.path.join(wc_dir, 'A', 'B', 'E', 'alpha') : None,
    os.path.join(wc_dir, 'A', 'B', 'E', 'beta') : None,
    os.path.join(wc_dir, 'A', 'B', 'lambda') : None,
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : None,
    os.path.join(wc_dir, 'A', 'D', 'gamma') : None,
    os.path.join(wc_dir, 'iota') : None,
    }
  verify_changelist_output(output, None, expected_removals)

  # svn changelist --remove WC_DIR --depth infinity
  #     --changelist i --changelist o
  output, errput = svntest.main.run_svn(None, "changelist", "--remove",
                                        "--depth", "infinity",
                                        "--changelist", "i",
                                        "--changelist", "o",
                                        wc_dir)
  expected_removals = {
    os.path.join(wc_dir, 'A', 'D', 'G', 'pi') : None,
    os.path.join(wc_dir, 'A', 'D', 'G', 'rho') : None,
    os.path.join(wc_dir, 'A', 'D', 'H', 'chi') : None,
    os.path.join(wc_dir, 'A', 'D', 'H', 'psi') : None,
    }
  verify_changelist_output(output, None, expected_removals)

#----------------------------------------------------------------------

def commit_one_changelist(sbox):
  "commit with single --changelist"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a line of text to all the versioned files in the tree.
  mod_all_files(wc_dir, "New text.\n")

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)
  
  # Now, test a commit that uses a single changelist filter (--changelist a).
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/lambda' : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    'A/B/E/beta' : Item(verb='Sending'),
    'A/D/gamma' : Item(verb='Sending'),
    'A/D/H/omega' : Item(verb='Sending'),
    'iota' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', 'A/D/G/tau', 'A/D/G/pi', 'A/D/H/chi',
                        'A/D/H/psi', 'A/D/G/rho', wc_rev=1, status='M ')
  expected_status.tweak('iota', 'A/B/lambda', 'A/B/E/alpha', 'A/B/E/beta',
                        'A/D/gamma', 'A/D/H/omega', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        wc_dir,
                                        "--changelist",
                                        "a")
  
#----------------------------------------------------------------------

def commit_multiple_changelists(sbox):
  "commit with multiple --changelist's"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a line of text to all the versioned files in the tree.
  mod_all_files(wc_dir, "New text.\n")

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)
  
  # Now, test a commit that uses multiple changelist filters
  # (--changelist=a --changelist=i).
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/lambda' : Item(verb='Sending'),
    'A/B/E/alpha' : Item(verb='Sending'),
    'A/B/E/beta' : Item(verb='Sending'),
    'A/D/gamma' : Item(verb='Sending'),
    'A/D/H/omega' : Item(verb='Sending'),
    'iota' : Item(verb='Sending'),
    'A/D/G/pi' : Item(verb='Sending'),
    'A/D/H/chi' : Item(verb='Sending'),
    'A/D/H/psi' : Item(verb='Sending'),
    })
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/mu', 'A/D/G/tau', 'A/D/G/rho',
                        wc_rev=1, status='M ')
  expected_status.tweak('iota', 'A/B/lambda', 'A/B/E/alpha', 'A/B/E/beta',
                        'A/D/gamma', 'A/D/H/omega', 'A/D/G/pi', 'A/D/H/chi',
                        'A/D/H/psi', wc_rev=2, status='  ')
  svntest.actions.run_and_verify_commit(wc_dir,
                                        expected_output,
                                        expected_status,
                                        None,
                                        wc_dir,
                                        "--changelist", "a",
                                        "--changelist", "i")

#----------------------------------------------------------------------

def info_with_changelists(sbox):
  "info --changelist"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)
  
  # Now, test various combinations of changelist specification and depths.
  for clname in [['a'], ['i'], ['a', 'i']]:
    for depth in [None, 'files', 'infinity']:

      # Figure out what we expect to see in our info output.
      expected_paths = []
      if 'a' in clname:
        if depth == 'infinity':
          expected_paths.append('A/B/lambda')
          expected_paths.append('A/B/E/alpha')
          expected_paths.append('A/B/E/beta')
          expected_paths.append('A/D/gamma')
          expected_paths.append('A/D/H/omega')
        if depth == 'files' or depth == 'infinity':
          expected_paths.append('iota')
      if 'i' in clname:
        if depth == 'infinity':
          expected_paths.append('A/D/G/pi')
          expected_paths.append('A/D/H/chi')
          expected_paths.append('A/D/H/psi')
      expected_paths = map(lambda x:
                           os.path.join(wc_dir, x.replace('/', os.sep)),
                           expected_paths)
      expected_paths.sort()
          
      # Build the command line.
      args = ['info', wc_dir]
      for cl in clname:
        args.append('--changelist')
        args.append(cl)
      if depth:
        args.append('--depth')
        args.append(depth)

      # Run 'svn info ...'
      output, errput = svntest.main.run_svn(None, *args)

      # Filter the output for lines that begin with 'Path:', and
      # reduce even those lines to just the actual path.
      def startswith_path(line):
        return line[:6] == 'Path: ' and 1 or 0
      paths = map(lambda x: x[6:].rstrip(), filter(startswith_path, output))
      paths.sort()

      # And, compare!
      if (paths != expected_paths):
        raise svntest.Failure("Expected paths (%s) and actual paths (%s) "
                              "don't gel" % (str(expected_paths), str(paths)))
      
#----------------------------------------------------------------------

def diff_with_changelists(sbox):
  "diff --changelist (wc-wc and repos-wc)"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a line of text to all the versioned files in the tree.
  mod_all_files(wc_dir, "New text.\n")

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)
  
  # Now, test various combinations of changelist specification and depths.
  for is_repos_wc in [0, 1]:
    for clname in [['a'], ['i'], ['a', 'i']]:
      for depth in ['files', 'infinity']:

        # Figure out what we expect to see in our diff output.
        expected_paths = []
        if 'a' in clname:
          if depth == 'infinity':
            expected_paths.append('A/B/lambda')
            expected_paths.append('A/B/E/alpha')
            expected_paths.append('A/B/E/beta')
            expected_paths.append('A/D/gamma')
            expected_paths.append('A/D/H/omega')
          if depth == 'files' or depth == 'infinity':
            expected_paths.append('iota')
        if 'i' in clname:
          if depth == 'infinity':
            expected_paths.append('A/D/G/pi')
            expected_paths.append('A/D/H/chi')
            expected_paths.append('A/D/H/psi')
        expected_paths = map(lambda x:
                             os.path.join(wc_dir, x.replace('/', os.sep)),
                             expected_paths)
        expected_paths.sort()
            
        # Build the command line.
        args = ['diff']
        for cl in clname:
          args.append('--changelist')
          args.append(cl)
        if depth:
          args.append('--depth')
          args.append(depth)
        if is_repos_wc:
          args.append('--old')
          args.append(sbox.repo_url)
          args.append('--new')
          args.append(sbox.wc_dir)
        else:
          args.append(wc_dir)
  
        # Run 'svn diff ...'
        output, errput = svntest.main.run_svn(None, *args)
  
        # Filter the output for lines that begin with 'Index:', and
        # reduce even those lines to just the actual path.
        def startswith_path(line):
          return line[:7] == 'Index: ' and 1 or 0
        paths = map(lambda x: x[7:].rstrip(), filter(startswith_path, output))
        paths.sort()

        # Diff output on Win32 uses '/' path separators.
        if sys.platform == 'win32':
          paths = map(lambda x:
                      x.replace('/', os.sep),
                      paths)

        # And, compare!
        if (paths != expected_paths):
          raise svntest.Failure("Expected paths (%s) and actual paths (%s) "
                                "don't gel"
                                % (str(expected_paths), str(paths)))

#----------------------------------------------------------------------

def propmods_with_changelists(sbox):
  "propset/del/get --changelist"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)

  # Set property 'name'='value' on all working copy items.
  svntest.main.run_svn(None, "pset", "--depth", "infinity",
                       "name", "value", wc_dir)
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.add({'' : Item(props={ 'name' : 'value' })})
  expected_disk.tweak('A', 'A/B', 'A/B/E', 'A/B/E/alpha', 'A/B/E/beta',
                      'A/B/F', 'A/B/lambda', 'A/C', 'A/D', 'A/D/G',
                      'A/D/G/pi', 'A/D/G/rho', 'A/D/G/tau', 'A/D/H',
                      'A/D/H/chi', 'A/D/H/omega', 'A/D/H/psi', 'A/D/gamma',
                      'A/mu', 'iota', props={ 'name' : 'value' })
  actual_disk_tree = svntest.tree.build_tree_from_wc(wc_dir, 1)
  svntest.tree.compare_trees("disk", actual_disk_tree,
                             expected_disk.old_tree())

  # Remove the 'name' property from files in the 'o' and 'i' changelists.
  svntest.main.run_svn(None, "pdel", "--depth", "infinity",
                       "name", "--changelist", "o", "--changelist", "i",
                       wc_dir)
  expected_disk.tweak('A/D/G/pi', 'A/D/G/rho', 'A/D/H/chi', 'A/D/H/psi',
                      props={})
  actual_disk_tree = svntest.tree.build_tree_from_wc(wc_dir, 1)
  svntest.tree.compare_trees("disk", actual_disk_tree,
                             expected_disk.old_tree())

  # Add 'foo'='bar' property on all files under A/B to depth files and
  # in changelist 'a'.
  svntest.main.run_svn(None, "pset", "--depth", "files",
                       "foo", "bar", "--changelist", "a",
                       os.path.join(wc_dir, 'A', 'B'))
  expected_disk.tweak('A/B/lambda', props={ 'name' : 'value',
                                            'foo'  : 'bar' })
  actual_disk_tree = svntest.tree.build_tree_from_wc(wc_dir, 1)
  svntest.tree.compare_trees("disk", actual_disk_tree,
                             expected_disk.old_tree())
  
  # Add 'bloo'='blarg' property to all files in changelist 'a'.
  svntest.main.run_svn(None, "pset", "--depth", "infinity",
                       "bloo", "blarg", "--changelist", "a",
                       wc_dir)
  expected_disk.tweak('A/B/lambda', props={ 'name' : 'value',
                                            'foo'  : 'bar',
                                            'bloo' : 'blarg' })
  expected_disk.tweak('A/B/E/alpha', 'A/B/E/beta', 'A/D/H/omega', 'A/D/gamma',
                      'iota', props={ 'name' : 'value',
                                      'bloo' : 'blarg' })
  actual_disk_tree = svntest.tree.build_tree_from_wc(wc_dir, 1)
  svntest.tree.compare_trees("disk", actual_disk_tree,
                             expected_disk.old_tree())

  # Propget 'name' in files in changelists 'a' and 'i' to depth files.
  output, errput = svntest.main.run_svn(None, "pget",
                                        "--depth", "files", "name",
                                        "--changelist", "a",
                                        "--changelist", "i",
                                        wc_dir)
  verify_pget_output(output, {
    os.path.join(wc_dir, 'iota') : 'value',
    })
  
  # Propget 'name' in files in changelists 'a' and 'i' to depth infinity.
  output, errput = svntest.main.run_svn(None, "pget",
                                        "--depth", "infinity", "name",
                                        "--changelist", "a",
                                        "--changelist", "i",
                                        wc_dir)
  verify_pget_output(output, {
    os.path.join(wc_dir, 'A', 'D', 'gamma')      : 'value',
    os.path.join(wc_dir, 'A', 'B', 'E', 'alpha') : 'value',
    os.path.join(wc_dir, 'iota')                 : 'value',
    os.path.join(wc_dir, 'A', 'B', 'E', 'beta')  : 'value',
    os.path.join(wc_dir, 'A', 'B', 'lambda')     : 'value',
    os.path.join(wc_dir, 'A', 'D', 'H', 'omega') : 'value',
    })
 

#----------------------------------------------------------------------

def revert_with_changelists(sbox):
  "revert --changelist"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)
  
  # Add a line of text to all the versioned files in the tree.
  mod_all_files(wc_dir, "Please, oh please, revert me!\n")
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  expected_status.tweak('A/B/lambda', 'A/B/E/alpha', 'A/B/E/beta',
                        'A/D/gamma', 'A/D/H/omega', 'iota', 'A/mu',
                        'A/D/G/tau', 'A/D/G/pi', 'A/D/H/chi',
                        'A/D/H/psi', 'A/D/G/rho', status='M ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # 'svn revert --changelist a WC_DIR' (without depth, no change expected)
  svntest.main.run_svn(None, "revert", "--changelist", "a", wc_dir)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # 'svn revert --changelist o --depth files WC_DIR WC_DIR/A/B' (no change)
  svntest.main.run_svn(None, "revert", "--depth", "files",
                       "--changelist", "o",
                       wc_dir, os.path.join(wc_dir, 'A', 'B'))
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # 'svn revert --changelist a --depth files WC_DIR WC_DIR/A/B'
  # (iota, lambda reverted)
  svntest.main.run_svn(None, "revert", "--depth", "files",
                       "--changelist", "a",
                       wc_dir, os.path.join(wc_dir, 'A', 'B'))
  expected_status.tweak('iota', 'A/B/lambda', status='  ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # 'svn revert --changelist a --changelist i --depth infinity WC_DIR'
  # (alpha, beta, gamma, omega, pi, chi, psi reverted)
  svntest.main.run_svn(None, "revert", "--depth", "infinity",
                       "--changelist", "a", "--changelist", "i",
                       wc_dir)
  expected_status.tweak('A/B/E/alpha', 'A/B/E/beta', 'A/D/gamma',
                        'A/D/H/omega', 'A/D/G/pi', 'A/D/H/chi',
                        'A/D/H/psi', status='  ')
  svntest.actions.run_and_verify_status(wc_dir, expected_status)

  # 'svn revert --depth infinity WC_DIR' (back to pristine-ness)
  svntest.main.run_svn(None, "revert", "--depth", "infinity",
                       wc_dir)
  expected_status = svntest.actions.get_virginal_state(wc_dir, 1)
  svntest.actions.run_and_verify_status(wc_dir, expected_status)
  
#----------------------------------------------------------------------

def update_with_changelists(sbox):
  "update --changelist"

  sbox.build()
  wc_dir = sbox.wc_dir

  # Add a line of text to all the versioned files in the tree, commit, update.
  mod_all_files(wc_dir, "Added line.\n")
  svntest.main.run_svn(None, "commit", "-m", "logmsg", wc_dir)
  svntest.main.run_svn(None, "update", wc_dir)

  # Add files to changelists based on the last character in their names.
  changelist_all_files(wc_dir, clname_from_lastchar_cb)

  ### Backdate only the files in the 'a' and 'i' changelists at depth
  ### files under WC_DIR and WC_DIR/A/B.

  # We expect update to only touch lambda and iota.
  expected_output = svntest.wc.State(wc_dir, {
    'A/B/lambda' : Item(status='U '),
    'iota' : Item(status='U '),
    })

  # Disk state should have all the files except iota and lambda
  # carrying new text.
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.tweak('A/B/E/alpha',
                      contents="This is the file 'alpha'.\nAdded line.\n")
  expected_disk.tweak('A/B/E/beta',
                      contents="This is the file 'beta'.\nAdded line.\n")
  expected_disk.tweak('A/D/gamma',
                      contents="This is the file 'gamma'.\nAdded line.\n")
  expected_disk.tweak('A/D/H/omega',
                      contents="This is the file 'omega'.\nAdded line.\n")
  expected_disk.tweak('A/mu',
                      contents="This is the file 'mu'.\nAdded line.\n")
  expected_disk.tweak('A/D/G/tau',
                      contents="This is the file 'tau'.\nAdded line.\n")
  expected_disk.tweak('A/D/G/pi',
                      contents="This is the file 'pi'.\nAdded line.\n")
  expected_disk.tweak('A/D/H/chi',
                      contents="This is the file 'chi'.\nAdded line.\n")
  expected_disk.tweak('A/D/H/psi',
                      contents="This is the file 'psi'.\nAdded line.\n")
  expected_disk.tweak('A/D/G/rho',
                      contents="This is the file 'rho'.\nAdded line.\n")

  # Status is clean, but with iota and lambda at r1 and all else at r2.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak('iota', 'A/B/lambda', wc_rev=1)

  # Update.
  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None,
                                        None, None, 1,
                                        "-r", "1",
                                        "--changelist", "a",
                                        "--changelist", "i",
                                        "--depth", "files",
                                        wc_dir,
                                        os.path.join(wc_dir, 'A', 'B'))

  ### Backdate to depth infinity all changelists "a", "i", and "o" now.

  # We expect update to only touch all the files ending in 'a', 'i',
  # and 'o' (except lambda and iota which were previously updated).
  expected_output = svntest.wc.State(wc_dir, {
    'A/D/G/pi' : Item(status='U '),
    'A/D/H/chi' : Item(status='U '),
    'A/D/H/psi' : Item(status='U '),
    'A/D/G/rho' : Item(status='U '),
    'A/B/E/alpha' : Item(status='U '),
    'A/B/E/beta' : Item(status='U '),
    'A/D/gamma' : Item(status='U '),
    'A/D/H/omega' : Item(status='U '),
    })

  # Disk state should have only tau and mu carrying new text.
  expected_disk = svntest.main.greek_state.copy()
  expected_disk.tweak('A/mu',
                      contents="This is the file 'mu'.\nAdded line.\n")
  expected_disk.tweak('A/D/G/tau',
                      contents="This is the file 'tau'.\nAdded line.\n")

  # Status is clean, but with iota and lambda at r1 and all else at r2.
  expected_status = svntest.actions.get_virginal_state(wc_dir, 2)
  expected_status.tweak('iota', 'A/B/lambda', 'A/D/G/pi', 'A/D/H/chi',
                        'A/D/H/psi', 'A/D/G/rho', 'A/B/E/alpha',
                        'A/B/E/beta', 'A/D/gamma', 'A/D/H/omega', wc_rev=1)

  # Update.
  svntest.actions.run_and_verify_update(wc_dir,
                                        expected_output,
                                        expected_disk,
                                        expected_status,
                                        None, None, None,
                                        None, None, 1,
                                        "-r", "1",
                                        "--changelist", "a",
                                        "--changelist", "i",
                                        "--changelist", "o",
                                        "--depth", "infinity",
                                        wc_dir)


########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              add_remove_changelists,
              commit_one_changelist,
              commit_multiple_changelists,
              info_with_changelists,
              diff_with_changelists,
              propmods_with_changelists,
              revert_with_changelists,
              update_with_changelists,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

#
#  main.py: a shared, automated test suite for Subversion
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2000-2007 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import sys     # for argv[]
import os
import shutil  # for rmtree()
import re
import stat    # for ST_MODE
import copy    # for deepcopy()
import time    # for time()
import traceback # for print_exc()
import threading
import Queue

import getopt
try:
  my_getopt = getopt.gnu_getopt
except AttributeError:
  my_getopt = getopt.getopt

from svntest import Failure
from svntest import Skip
from svntest import testcase
from svntest import wc

######################################################################
#
#  HOW TO USE THIS MODULE:
#
#  Write a new python script that
#
#     1) imports this 'svntest' package
#
#     2) contains a number of related 'test' routines.  (Each test
#        routine should take no arguments, and return None on success
#        or throw a Failure exception on failure.  Each test should
#        also contain a short docstring.)
#
#     3) places all the tests into a list that begins with None.
#
#     4) calls svntest.main.client_test() on the list.
#
#  Also, your tests will probably want to use some of the common
#  routines in the 'Utilities' section below.
#
#####################################################################
# Global stuff

class SVNProcessTerminatedBySignal(Failure):
  "Exception raised if a spawned process segfaulted, aborted, etc."
  pass

class SVNLineUnequal(Failure):
  "Exception raised if two lines are unequal"
  pass

class SVNUnmatchedError(Failure):
  "Exception raised if an expected error is not found"
  pass

class SVNCommitFailure(Failure):
  "Exception raised if a commit failed"
  pass

class SVNRepositoryCopyFailure(Failure):
  "Exception raised if unable to copy a repository"
  pass

class SVNRepositoryCreateFailure(Failure):
  "Exception raised if unable to create a repository"
  pass

# Windows specifics
if sys.platform == 'win32':
  windows = True
  file_scheme_prefix = 'file:///'
  _exe = '.exe'
  _bat = '.bat'
else:
  windows = False
  file_scheme_prefix = 'file://'
  _exe = ''
  _bat = ''

try:
  from popen2 import Popen3
  platform_with_popen3_class = True
except ImportError:
  platform_with_popen3_class = False

# The location of our mock svneditor script.
if sys.platform == 'win32':
  svneditor_script = os.path.join(sys.path[0], 'svneditor.bat')
else:
  svneditor_script = os.path.join(sys.path[0], 'svneditor.py')

# Username and password used by the working copies
wc_author = 'jrandom'
wc_passwd = 'rayjandom'

# Username and password used by the working copies for "second user"
# scenarios
wc_author2 = 'jconstant' # use the same password as wc_author

######################################################################
# Global variables set during option parsing.  These should not be used
# until the variable command_line_parsed has been set to True, as is
# done in run_tests below.
command_line_parsed = False

# The locations of the svn, svnadmin and svnlook binaries, relative to
# the only scripts that import this file right now (they live in ../).
# Use --bin to override these defaults.
svn_binary = os.path.abspath('../../svn/svn' + _exe)
svnadmin_binary = os.path.abspath('../../svnadmin/svnadmin' + _exe)
svnlook_binary = os.path.abspath('../../svnlook/svnlook' + _exe)
svnsync_binary = os.path.abspath('../../svnsync/svnsync' + _exe)
svnversion_binary = os.path.abspath('../../svnversion/svnversion' + _exe)
svndumpfilter_binary = os.path.abspath('../../svndumpfilter/svndumpfilter' + \
                                       _exe)

# Global variable indicating if we want verbose output, that is,
# details of what commands each test does as it does them.  This is
# incompatible with quiet_mode.
verbose_mode = False

# Global variable indicating if we want quiet output, that is, don't
# show PASS, XFAIL, or SKIP notices, but do show FAIL and XPASS.  This
# is incompatible with verbose_mode.
quiet_mode = False

# Global variable indicating if we want test data cleaned up after success
cleanup_mode = False

# Global variable indicating if svnserve should use Cyrus SASL
enable_sasl = False

# Global variable indicating which DAV library, if any, is in use
# ('neon', 'serf')
http_library = None

# Global variable indicating what the minor version of the server
# tested against is (4 for 1.4.x, for example).
server_minor_version = 5

# Global variable indicating if this is a child process and no cleanup
# of global directories is needed.
is_child_process = False

# Global URL to testing area.  Default to ra_local, current working dir.
test_area_url = file_scheme_prefix + os.path.abspath(os.getcwd())
if windows:
  test_area_url = test_area_url.replace('\\', '/')

# Location to the pristine repository, will be calculated from test_area_url
# when we know what the user specified for --url.
pristine_url = None

# Global variable indicating the FS type for repository creations.
fs_type = None

# End of command-line-set global variables.
######################################################################

# All temporary repositories and working copies are created underneath
# this dir, so there's one point at which to mount, e.g., a ramdisk.
work_dir = "svn-test-work"

# Constant for the merge info property.
SVN_PROP_MERGE_INFO = "svn:mergeinfo"

# Where we want all the repositories and working copies to live.
# Each test will have its own!
general_repo_dir = os.path.join(work_dir, "repositories")
general_wc_dir = os.path.join(work_dir, "working_copies")

# temp directory in which we will create our 'pristine' local
# repository and other scratch data.  This should be removed when we
# quit and when we startup.
temp_dir = os.path.join(work_dir, 'local_tmp')

# (derivatives of the tmp dir.)
pristine_dir = os.path.join(temp_dir, "repos")
greek_dump_dir = os.path.join(temp_dir, "greekfiles")
default_config_dir = os.path.abspath(os.path.join(temp_dir, "config"))

#
# Our pristine greek-tree state.
#
# If a test wishes to create an "expected" working-copy tree, it should
# call main.greek_state.copy().  That method will return a copy of this
# State object which can then be edited.
#
_item = wc.StateItem
greek_state = wc.State('', {
  'iota'        : _item("This is the file 'iota'.\n"),
  'A'           : _item(),
  'A/mu'        : _item("This is the file 'mu'.\n"),
  'A/B'         : _item(),
  'A/B/lambda'  : _item("This is the file 'lambda'.\n"),
  'A/B/E'       : _item(),
  'A/B/E/alpha' : _item("This is the file 'alpha'.\n"),
  'A/B/E/beta'  : _item("This is the file 'beta'.\n"),
  'A/B/F'       : _item(),
  'A/C'         : _item(),
  'A/D'         : _item(),
  'A/D/gamma'   : _item("This is the file 'gamma'.\n"),
  'A/D/G'       : _item(),
  'A/D/G/pi'    : _item("This is the file 'pi'.\n"),
  'A/D/G/rho'   : _item("This is the file 'rho'.\n"),
  'A/D/G/tau'   : _item("This is the file 'tau'.\n"),
  'A/D/H'       : _item(),
  'A/D/H/chi'   : _item("This is the file 'chi'.\n"),
  'A/D/H/psi'   : _item("This is the file 'psi'.\n"),
  'A/D/H/omega' : _item("This is the file 'omega'.\n"),
  })


######################################################################
# Utilities shared by the tests
def wrap_ex(func):
  "Wrap a function, catch, print and ignore exceptions"
  def w(*args, **kwds):
    try:
      return func(*args, **kwds)
    except Failure, ex:
      if ex.__class__ != Failure or ex.args:
        ex_args = str(ex)
        if ex_args:
          print 'EXCEPTION: %s: %s' % (ex.__class__.__name__, ex_args)
        else:
          print 'EXCEPTION:', ex.__class__.__name__
  return w

def setup_design_mode():
  "Wraps functions in module actions"
  l = [ 'run_and_verify_svn',
        'run_and_verify_svnversion',
        'run_and_verify_load',
        'run_and_verify_dump',
        'run_and_verify_checkout',
        'run_and_verify_export',
        'run_and_verify_update',
        'run_and_verify_merge',
        'run_and_verify_merge2',
        'run_and_verify_switch',
        'run_and_verify_commit',
        'run_and_verify_unquiet_status',
        'run_and_verify_status',
        'run_and_verify_diff_summarize',
        'run_and_verify_diff_summarize_xml',
        'run_and_validate_lock']
  
  for func in l:
    setattr(actions, func, wrap_ex(getattr(actions, func)))

def get_admin_name():
  "Return name of SVN administrative subdirectory."

  if (windows or sys.platform == 'cygwin') \
      and os.environ.has_key('SVN_ASP_DOT_NET_HACK'):
    return '_svn'
  else:
    return '.svn'

def get_start_commit_hook_path(repo_dir):
  "Return the path of the start-commit-hook conf file in REPO_DIR."

  return os.path.join(repo_dir, "hooks", "start-commit")


def get_pre_commit_hook_path(repo_dir):
  "Return the path of the pre-commit-hook conf file in REPO_DIR."

  return os.path.join(repo_dir, "hooks", "pre-commit")


def get_post_commit_hook_path(repo_dir):
  "Return the path of the post-commit-hook conf file in REPO_DIR."

  return os.path.join(repo_dir, "hooks", "post-commit")

def get_pre_revprop_change_hook_path(repo_dir):
  "Return the path of the pre-revprop-change hook script in REPO_DIR."

  return os.path.join(repo_dir, "hooks", "pre-revprop-change")

def get_svnserve_conf_file_path(repo_dir):
  "Return the path of the svnserve.conf file in REPO_DIR."

  return os.path.join(repo_dir, "conf", "svnserve.conf")

# Run any binary, logging the command line (TODO: and return code)
def run_command(command, error_expected, binary_mode=0, *varargs):
  """Run COMMAND with VARARGS; return stdout, stderr as lists of lines.
  If ERROR_EXPECTED is None, any stderr also will be printed."""

  return run_command_stdin(command, error_expected, binary_mode,
                           None, *varargs)

# A regular expression that matches arguments that are trivially safe
# to pass on a command line without quoting on any supported operating
# system:
_safe_arg_re = re.compile(r'^[A-Za-z\d\.\_\/\-\:\@]+$')

def _quote_arg(arg):
  """Quote ARG for a command line.

  Simply surround every argument in double-quotes unless it contains
  only universally harmless characters.

  WARNING: This function cannot handle arbitrary command-line
  arguments.  It can easily be confused by shell metacharacters.  A
  perfect job would be difficult and OS-dependent (see, for example,
  http://msdn.microsoft.com/library/en-us/vccelng/htm/progs_12.asp).
  In other words, this function is just good enough for what we need
  here."""

  arg = str(arg)
  if _safe_arg_re.match(arg):
    return arg
  else:
    if os.name != 'nt':
      arg = arg.replace('$', '\$')
    return '"%s"' % (arg,)

def open_pipe(command, mode):
  """Opens a popen3 pipe to COMMAND in MODE.

  Returns (infile, outfile, errfile, waiter); waiter
  should be passed to wait_on_pipe."""
  if platform_with_popen3_class:
    kid = Popen3(command, True)
    return kid.tochild, kid.fromchild, kid.childerr, (kid, command)
  else:
    inf, outf, errf = os.popen3(command, mode)
    return inf, outf, errf, None

def wait_on_pipe(waiter, stdout_lines, stderr_lines):
  """Waits for KID (opened with open_pipe) to finish, dying
  if it does.  Uses STDOUT_LINES and STDERR_LINES for error message
  if kid fails.  Returns kid's exit code."""
  if waiter is None:
    return

  kid, command = waiter

  wait_code = kid.wait()

  if os.WIFSIGNALED(wait_code):
    exit_signal = os.WTERMSIG(wait_code)
    if stdout_lines is not None:
      sys.stdout.write("".join(stdout_lines))
    if stderr_lines is not None:
      sys.stderr.write("".join(stderr_lines))
    if verbose_mode:
      # show the whole path to make it easier to start a debugger
      sys.stderr.write("CMD: %s terminated by signal %d\n"
                       % (command, exit_signal))
    raise SVNProcessTerminatedBySignal
  else:
    exit_code = os.WEXITSTATUS(wait_code)
    if exit_code and verbose_mode:
      sys.stderr.write("CMD: %s exited with %d\n" % (command, exit_code))
    return exit_code

# Run any binary, supplying input text, logging the command line
def spawn_process(command, binary_mode=0,stdin_lines=None, *varargs):
  args = ' '.join(map(_quote_arg, varargs))

  # Log the command line
  if verbose_mode and not command.endswith('.py'):
    print 'CMD:', os.path.basename(command) + ' ' + args,

  if binary_mode:
    mode = 'b'
  else:
    mode = 't'

  infile, outfile, errfile, kid = open_pipe(command + ' ' + args, mode)

  if stdin_lines:
    map(infile.write, stdin_lines)

  infile.close()

  stdout_lines = outfile.readlines()
  stderr_lines = errfile.readlines()

  outfile.close()
  errfile.close()

  exit_code = wait_on_pipe(kid, stdout_lines, stderr_lines)

  return exit_code, stdout_lines, stderr_lines

def run_command_stdin(command, error_expected, binary_mode=0,
                      stdin_lines=None, *varargs):
  """Run COMMAND with VARARGS; input STDIN_LINES (a list of strings
  which should include newline characters) to program via stdin - this
  should not be very large, as if the program outputs more than the OS
  is willing to buffer, this will deadlock, with both Python and
  COMMAND waiting to write to each other for ever.
  Return stdout, stderr as lists of lines.
  If ERROR_EXPECTED is None, any stderr also will be printed."""

  if verbose_mode:
    start = time.time()

  exit_code, stdout_lines, stderr_lines = spawn_process(command,
                                                        binary_mode,
                                                        stdin_lines,
                                                        *varargs)

  if verbose_mode:
    stop = time.time()
    print '<TIME = %.6f>' % (stop - start)

  if (not error_expected) and (stderr_lines):
    map(sys.stdout.write, stderr_lines)
    raise Failure

  return stdout_lines, stderr_lines

def create_config_dir(cfgdir, config_contents=None, server_contents=None):
  "Create config directories and files"

  # config file names
  cfgfile_cfg = os.path.join(cfgdir, 'config')
  cfgfile_srv = os.path.join(cfgdir, 'servers')

  # create the directory
  if not os.path.isdir(cfgdir):
    os.makedirs(cfgdir)

  # define default config file contents if none provided
  if config_contents is None:
    config_contents = """
#
[miscellany]
interactive-conflicts = false
"""

  # define default server file contents if none provided
  if server_contents is None:
    if http_library:
      server_contents = """
#
[global]
http-library=%s
""" % (http_library)
    else:
      server_contents = "#\n"

  file_write(cfgfile_cfg, config_contents)
  file_write(cfgfile_srv, server_contents)

def _with_config_dir(args):
  if '--config-dir' in args:
    return args
  else:
    return args + ('--config-dir', default_config_dir)

def _with_auth(args):
  assert '--password' not in args
  args = args + ('--password', wc_passwd,
                 '--no-auth-cache' )
  if '--username' in args:
    return args
  else:
    return args + ('--username', wc_author )

# For running subversion and returning the output
def run_svn(error_expected, *varargs):
  """Run svn with VARARGS; return stdout, stderr as lists of lines.
  If ERROR_EXPECTED is None, any stderr also will be printed.  If
  you're just checking that something does/doesn't come out of
  stdout/stderr, you might want to use actions.run_and_verify_svn()."""
  return run_command(svn_binary, error_expected, 0,
                     *(_with_auth(_with_config_dir(varargs))))

# For running svnadmin.  Ignores the output.
def run_svnadmin(*varargs):
  "Run svnadmin with VARARGS, returns stdout, stderr as list of lines."
  return run_command(svnadmin_binary, 1, 0, *varargs)

# For running svnlook.  Ignores the output.
def run_svnlook(*varargs):
  "Run svnlook with VARARGS, returns stdout, stderr as list of lines."
  return run_command(svnlook_binary, 1, 0, *varargs)

def run_svnsync(*varargs):
  "Run svnsync with VARARGS, returns stdout, stderr as list of lines."
  return run_command(svnsync_binary, 1, 0, *(_with_config_dir(varargs)))

def run_svnversion(*varargs):
  "Run svnversion with VARARGS, returns stdout, stderr as list of lines."
  return run_command(svnversion_binary, 1, 0, *varargs)

# Chmod recursively on a whole subtree
def chmod_tree(path, mode, mask):
  def visit(arg, dirname, names):
    mode, mask = arg
    for name in names:
      fullname = os.path.join(dirname, name)
      if not os.path.islink(fullname):
        new_mode = (os.stat(fullname)[stat.ST_MODE] & ~mask) | mode
        os.chmod(fullname, new_mode)
  os.path.walk(path, visit, (mode, mask))

# For clearing away working copies
def safe_rmtree(dirname, retry=0):
  "Remove the tree at DIRNAME, making it writable first"
  def rmtree(dirname):
    chmod_tree(dirname, 0666, 0666)
    shutil.rmtree(dirname)

  if not os.path.exists(dirname):
    return

  if retry:
    for delay in (0.5, 1, 2, 4):
      try:
        rmtree(dirname)
        break
      except:
        time.sleep(delay)
    else:
      rmtree(dirname)
  else:
    rmtree(dirname)

# For making local mods to files
def file_append(path, new_text):
  "Append NEW_TEXT to file at PATH"
  file_write(path, new_text, 'a')  # open in (a)ppend mode

# Append in binary mode
def file_append_binary(path, new_text):
  "Append NEW_TEXT to file at PATH in binary mode"
  file_write(path, new_text, 'ab')  # open in (a)ppend mode

# For creating new files, and making local mods to existing files.
def file_write(path, contents, mode = 'w'):
  """Write the CONTENTS to the file at PATH, opening file using MODE,
  which is (w)rite by default."""
  fp = open(path, mode)
  fp.write(contents)
  fp.close()

# For reading the contents of a file
def file_read(path, mode = 'r'):
  """Return the contents of the file at PATH, opening file using MODE,
  which is (r)ead by default."""
  fp = open(path, mode)
  contents = fp.read()
  fp.close()
  return contents

# For replacing parts of contents in an existing file, with new content.
def file_substitute(path, contents, new_contents):
  """Replace the CONTENTS in the file at PATH using the NEW_CONTENTS"""
  fp = open(path, 'r')
  fcontent = fp.read()
  fp.close()
  fcontent = fcontent.replace(contents, new_contents)
  fp = open(path, 'w')
  fp.write(fcontent)
  fp.close()

# For creating blank new repositories
def create_repos(path):
  """Create a brand-new SVN repository at PATH.  If PATH does not yet
  exist, create it."""

  if not os.path.exists(path):
    os.makedirs(path) # this creates all the intermediate dirs, if neccessary

  opts = ("--bdb-txn-nosync",)
  if server_minor_version < 5:
    opts += ("--pre-1.5-compatible",)
  if fs_type is not None:
    opts += ("--fs-type=" + fs_type,)
  stdout, stderr = run_command(svnadmin_binary, 1, 0, "create", path, *opts)

  # Skip tests if we can't create the repository.
  if stderr:
    for line in stderr:
      if line.find('Unknown FS type') != -1:
        raise Skip
    # If the FS type is known, assume the repos couldn't be created
    # (e.g. due to a missing 'svnadmin' binary).
    raise SVNRepositoryCreateFailure("".join(stderr).rstrip())

  # Allow unauthenticated users to write to the repos, for ra_svn testing.
  file_write(get_svnserve_conf_file_path(path),
             "[general]\nauth-access = write\n");
  if enable_sasl:
    file_append(get_svnserve_conf_file_path(path),
                "realm = svntest\n[sasl]\nuse-sasl = true\n")
  else:
    file_append(get_svnserve_conf_file_path(path), "password-db = passwd\n")
    file_append(os.path.join(path, "conf", "passwd"),
                "[users]\njrandom = rayjandom\njconstant = rayjandom\n");
  # make the repos world-writeable, for mod_dav_svn's sake.
  chmod_tree(path, 0666, 0666)

# For copying a repository
def copy_repos(src_path, dst_path, head_revision, ignore_uuid = 1):
  "Copy the repository SRC_PATH, with head revision HEAD_REVISION, to DST_PATH"

  # Do an svnadmin dump|svnadmin load cycle. Print a fake pipe command so that
  # the displayed CMDs can be run by hand
  create_repos(dst_path)
  dump_args = ' dump "' + src_path + '"'
  load_args = ' load "' + dst_path + '"'

  if ignore_uuid:
    load_args = load_args + " --ignore-uuid"
  if verbose_mode:
    print 'CMD:', os.path.basename(svnadmin_binary) + dump_args, \
          '|', os.path.basename(svnadmin_binary) + load_args,
  start = time.time()

  dump_in, dump_out, dump_err, dump_kid = \
           open_pipe(svnadmin_binary + dump_args, 'b')
  load_in, load_out, load_err, load_kid = \
           open_pipe(svnadmin_binary + load_args, 'b')
  stop = time.time()
  if verbose_mode:
    print '<TIME = %.6f>' % (stop - start)

  while 1:
    data = dump_out.read(1024*1024)  # Arbitrary buffer size
    if data == "":
      break
    load_in.write(data)
  load_in.close() # Tell load we are done

  dump_lines = dump_err.readlines()
  load_lines = load_out.readlines()
  dump_in.close()
  dump_out.close()
  dump_err.close()
  load_out.close()
  load_err.close()
  # Wait on the pipes; ignore return code.
  wait_on_pipe(dump_kid, None, dump_lines)
  wait_on_pipe(load_kid, load_lines, None)

  dump_re = re.compile(r'^\* Dumped revision (\d+)\.\r?$')
  expect_revision = 0
  for dump_line in dump_lines:
    match = dump_re.match(dump_line)
    if not match or match.group(1) != str(expect_revision):
      print 'ERROR:  dump failed:', dump_line,
      raise SVNRepositoryCopyFailure
    expect_revision += 1
  if expect_revision != head_revision + 1:
    print 'ERROR:  dump failed; did not see revision', head_revision
    raise SVNRepositoryCopyFailure

  load_re = re.compile(r'^------- Committed revision (\d+) >>>\r?$')
  expect_revision = 1
  for load_line in load_lines:
    match = load_re.match(load_line)
    if match:
      if match.group(1) != str(expect_revision):
        print 'ERROR:  load failed:', load_line,
        raise SVNRepositoryCopyFailure
      expect_revision += 1
  if expect_revision != head_revision + 1:
    print 'ERROR:  load failed; did not see revision', head_revision
    raise SVNRepositoryCopyFailure


def canonicalize_url(input):
  "Canonicalize the url, if the scheme is unknown, returns intact input"

  m = re.match(r"^((file://)|((svn|svn\+ssh|http|https)(://)))", input)
  if m:
    scheme = m.group(1)
    return scheme + re.sub(r'//*', '/', input[len(scheme):])
  else:
    return input


def create_python_hook_script (hook_path, hook_script_code):
  """Create a Python hook script at HOOK_PATH with the specified
     HOOK_SCRIPT_CODE."""

  if sys.platform == 'win32':
    # Use an absolute path since the working directory is not guaranteed
    hook_path = os.path.abspath(hook_path)
    # Fill the python file.
    file_write ("%s.py" % hook_path, hook_script_code)
    # Fill the batch wrapper file.
    file_append ("%s.bat" % hook_path,
                 "@\"%s\" %s.py %%*\n" % (sys.executable, hook_path))
  else:
    # For all other platforms
    file_write (hook_path, "#!%s\n%s" % (sys.executable, hook_script_code))
    os.chmod (hook_path, 0755)

def write_restrictive_svnserve_conf(repo_dir, anon_access="none"):
  "Create a restrictive authz file ( no anynomous access )."

  fp = open(get_svnserve_conf_file_path(repo_dir), 'w')
  fp.write("[general]\nanon-access = %s\nauth-access = write\n"
           "authz-db = authz\n" % anon_access)
  if enable_sasl == 1:
    fp.write("realm = svntest\n[sasl]\nuse-sasl = true\n");
  else:
    fp.write("password-db = passwd\n")
  fp.close()

# Warning: because mod_dav_svn uses one shared authz file for all
# repositories, you *cannot* use write_authz_file in any test that
# might be run in parallel.
#
# write_authz_file can *only* be used in test suites which disable
# parallel execution at the bottom like so
#   if __name__ == '__main__':
#     svntest.main.run_tests(test_list, serial_only = True)
def write_authz_file(sbox, rules, sections=None):
  """Write an authz file to SBOX, appropriate for the RA method used,
with authorizations rules RULES mapping paths to strings containing
the rules. You can add sections SECTIONS (ex. groups, aliases...) with
an appropriate list of mappings.
"""
  fp = open(sbox.authz_file, 'w')
  if sbox.repo_url.startswith("http"):
    prefix = sbox.name + ":"
  else:
    prefix = ""
  if sections:
    for p, r in sections.items():
      fp.write("[%s]\n%s\n" % (p, r))

  for p, r in rules.items():
    fp.write("[%s%s]\n%s\n" % (prefix, p, r))
  fp.close()

def use_editor(func):
  os.environ['SVN_EDITOR'] = svneditor_script
  os.environ['SVN_MERGE'] = svneditor_script
  os.environ['SVNTEST_EDITOR_FUNC'] = func


def merge_notify_line(revstart=None, revend=None, same_URL=True):
  """Return an expected output line that describes the beginning of a
  merge operation on revisions REVSTART through REVEND.  Omit both
  REVSTART and REVEND for the case where the left and right sides of
  the merge are from different URLs."""
  if not same_URL:
    return "--- Merging differences between repository URLs into '.+':\n"
  if revend is None:
    if revstart is None:
      # The left and right sides of the merge are from different URLs.
      return "--- Merging differences between repository URLs into '.+':\n"
    elif revstart < 0:
      return "--- Reverse-merging r%ld into '.+':\n" % abs(revstart)
    else:
      return "--- Merging r%ld into '.+':\n" % revstart
  else:
    if revstart > revend:
      return "--- Reverse-merging r%ld through r%ld into '.+':\n" % (revstart,
                                                                     revend)
    else:
      return "--- Merging r%ld through r%ld into '.+':\n" % (revstart, revend)


######################################################################
# Functions which check the test configuration
# (useful for conditional XFails)

def _check_command_line_parsed():
  """Raise an exception if the command line has not yet been parsed."""
  if not command_line_parsed:
    raise Failure("Condition cannot be tested until command line is parsed")

def is_ra_type_dav():
  _check_command_line_parsed()
  return test_area_url.startswith('http')

def is_ra_type_svn():
  _check_command_line_parsed()
  return test_area_url.startswith('svn')

def is_ra_type_file():
  _check_command_line_parsed()
  return test_area_url.startswith('file')

def is_fs_type_fsfs():
  _check_command_line_parsed()
  # This assumes that fsfs is the default fs implementation.
  return fs_type == 'fsfs' or fs_type is None

def is_os_windows():
  return os.name == 'nt'

def is_posix_os():
  return os.name == 'posix'

def is_os_darwin():
  return sys.platform == 'darwin'

def server_has_mergeinfo():
  _check_command_line_parsed()
  return server_minor_version >= 5

def server_has_revprop_commit():
  _check_command_line_parsed()
  return server_minor_version >= 5

def server_sends_copyfrom_on_update():
  _check_command_line_parsed()
  return server_minor_version >= 5

def server_authz_has_aliases():
  _check_command_line_parsed()
  return server_minor_version >= 5


######################################################################
# Sandbox handling

class Sandbox:
  """Manages a sandbox (one or more repository/working copy pairs) for
  a test to operate within."""

  dependents = None

  def __init__(self, module, idx):
    self._set_name("%s-%d" % (module, idx))

  def _set_name(self, name):
    """A convenience method for renaming a sandbox, useful when
    working with multiple repositories in the same unit test."""
    self.name = name
    self.wc_dir = os.path.join(general_wc_dir, self.name)
    self.repo_dir = os.path.join(general_repo_dir, self.name)
    self.repo_url = test_area_url + '/' + self.repo_dir

    # For dav tests we need a single authz file which must be present,
    # so we recreate it each time a sandbox is created with some default
    # contents.
    if self.repo_url.startswith("http"):
      # this dir doesn't exist out of the box, so we may have to make it
      if not os.path.exists(work_dir):
        os.makedirs(work_dir)
      self.authz_file = os.path.join(work_dir, "authz")
      file_write(self.authz_file, "[/]\n* = rw\n")

    # For svnserve tests we have a per-repository authz file, and it
    # doesn't need to be there in order for things to work, so we don't
    # have any default contents.
    elif self.repo_url.startswith("svn"):
      self.authz_file = os.path.join(self.repo_dir, "conf", "authz")

    if windows:
      self.repo_url = self.repo_url.replace('\\', '/')
    self.test_paths = [self.wc_dir, self.repo_dir]

  def clone_dependent(self):
    """A convenience method for creating a near-duplicate of this
    sandbox, useful when working with multiple repositories in the
    same unit test.  Any necessary cleanup operations are triggered
    by cleanup of the original sandbox."""
    if not self.dependents:
      self.dependents = []
    self.dependents.append(copy.deepcopy(self))
    self.dependents[-1]._set_name("%s-%d" % (self.name, len(self.dependents)))
    return self.dependents[-1]

  def build(self, name = None, create_wc = True):
    if name != None:
      self._set_name(name)
    if actions.make_repo_and_wc(self, create_wc):
      raise Failure("Could not build repository and sandbox '%s'" % self.name)

  def add_test_path(self, path, remove=1):
    self.test_paths.append(path)
    if remove:
      safe_rmtree(path)

  def add_repo_path(self, suffix, remove=1):
    path = self.repo_dir + '.' + suffix
    url  = self.repo_url + '.' + suffix
    self.add_test_path(path, remove)
    return path, url

  def add_wc_path(self, suffix, remove=1):
    path = self.wc_dir + '.' + suffix
    self.add_test_path(path, remove)
    return path

  def cleanup_test_paths(self):
    "Clean up detritus from this sandbox, and any dependents."
    if self.dependents:
      # Recursively cleanup any dependent sandboxes.
      for sbox in self.dependents:
        sbox.cleanup_test_paths()
    for path in self.test_paths:
      _cleanup_test_path(path)


_deferred_test_paths = []
def _cleanup_deferred_test_paths():
  global _deferred_test_paths
  test_paths = _deferred_test_paths[:]
  _deferred_test_paths = []
  for path in test_paths:
    _cleanup_test_path(path, 1)

def _cleanup_test_path(path, retrying=None):
  if verbose_mode:
    if retrying:
      print "CLEANUP: RETRY:", path
    else:
      print "CLEANUP:", path
  try:
    safe_rmtree(path)
  except:
    if verbose_mode:
      print "WARNING: cleanup failed, will try again later"
    _deferred_test_paths.append(path)

class TestSpawningThread(threading.Thread):
  """A thread that runs test cases in their own processes.
  Receives test numbers to run from the queue, and saves results into
  the results field."""
  def __init__(self, queue):
    threading.Thread.__init__(self)
    self.queue = queue
    self.results = []

  def run(self):
    while True:
      try:
        next_index = self.queue.get_nowait()
      except Queue.Empty:
        return

      self.run_one(next_index)

  def run_one(self, index):
    command = sys.argv[0]

    args = []
    args.append(str(index))
    args.append('-c')
    # add some startup arguments from this process
    if fs_type:
      args.append('--fs-type=' + fs_type)
    if test_area_url:
      args.append('--url=' + test_area_url)
    if verbose_mode:
      args.append('-v')
    if cleanup_mode:
      args.append('--cleanup')
    if enable_sasl:
      args.append('--enable-sasl')
    if http_library:
      args.append('--http-library=' + http_library)
    if server_minor_version:
      args.append('--server-minor-version=' + str(server_minor_version))

    result, stdout_lines, stderr_lines = spawn_process(command, 1, None, *args)
    # don't trust the exitcode, will not be correct on Windows
    if filter(lambda x: x.startswith('FAIL: ') or x.startswith('XPASS: '),
              stdout_lines):
      result = 1
    self.results.append((index, result, stdout_lines, stderr_lines))
    sys.stdout.write('.')
    sys.stdout.flush()

class TestRunner:
  """Encapsulate a single test case (predicate), including logic for
  runing the test and test list output."""

  def __init__(self, func, index):
    self.pred = testcase.create_test_case(func)
    self.index = index

  def list(self):
    print " %2d     %-5s  %s" % (self.index,
                                 self.pred.list_mode(),
                                 self.pred.get_description())
    self.pred.check_description()

  def _print_name(self):
    print os.path.basename(sys.argv[0]), str(self.index) + ":", \
          self.pred.get_description()
    self.pred.check_description()

  def run(self):
    """Run self.pred and return the result.  The return value is
        - 0 if the test was successful
        - 1 if it errored in a way that indicates test failure
        - 2 if the test skipped
        """
    if self.pred.need_sandbox():
      # ooh! this function takes a sandbox argument
      sandbox = Sandbox(self.pred.get_sandbox_name(), self.index)
      kw = { 'sandbox' : sandbox }
    else:
      sandbox = None
      kw = {}

    # Explicitly set this so that commands that commit but don't supply a
    # log message will fail rather than invoke an editor.
    # Tests that want to use an editor should invoke svntest.main.use_editor.
    os.environ['SVN_EDITOR'] = ''
    os.environ['SVNTEST_EDITOR_FUNC'] = ''
    actions.no_sleep_for_timestamps()

    saved_dir = os.getcwd()
    try:
      rc = apply(self.pred.run, (), kw)
      if rc is not None:
        print 'STYLE ERROR in',
        self._print_name()
        print 'Test driver returned a status code.'
        sys.exit(255)
      result = 0
    except Skip, ex:
      result = 2
    except Failure, ex:
      result = 1
      # We captured Failure and its subclasses. We don't want to print
      # anything for plain old Failure since that just indicates test
      # failure, rather than relevant information. However, if there
      # *is* information in the exception's arguments, then print it.
      if ex.__class__ != Failure or ex.args:
        ex_args = str(ex)
        if ex_args:
          print 'EXCEPTION: %s: %s' % (ex.__class__.__name__, ex_args)
        else:
          print 'EXCEPTION:', ex.__class__.__name__
      traceback.print_exc(file=sys.stdout)
    except KeyboardInterrupt:
      print 'Interrupted'
      sys.exit(0)
    except SystemExit, ex:
      print 'EXCEPTION: SystemExit(%d), skipping cleanup' % ex.code
      print ex.code and 'FAIL: ' or 'PASS: ',
      self._print_name()
      raise
    except:
      result = 1
      print 'UNEXPECTED EXCEPTION:'
      traceback.print_exc(file=sys.stdout)

    os.chdir(saved_dir)
    result = self.pred.convert_result(result)
    (result_text, result_benignity) = self.pred.run_text(result)
    if not (quiet_mode and result_benignity):
      print result_text,
      self._print_name()
      sys.stdout.flush()
    if sandbox is not None and result != 1 and cleanup_mode:
      sandbox.cleanup_test_paths()
    return result

######################################################################
# Main testing functions

# These two functions each take a TEST_LIST as input.  The TEST_LIST
# should be a list of test functions; each test function should take
# no arguments and return a 0 on success, non-zero on failure.
# Ideally, each test should also have a short, one-line docstring (so
# it can be displayed by the 'list' command.)

# Func to run one test in the list.
def run_one_test(n, test_list, parallel = 0, finished_tests = None):
  """Run the Nth client test in TEST_LIST, return the result.

  If we're running the tests in parallel spawn the test in a new process.
  """

  if (n < 1) or (n > len(test_list) - 1):
    print "There is no test", `n` + ".\n"
    return 1

  # Run the test.
  if parallel:
    st = SpawnTest(n, finished_tests)
    st.start()
    return 0
  else:
    exit_code = TestRunner(test_list[n], n).run()
    return exit_code

def _internal_run_tests(test_list, testnums, parallel):
  """Run the tests from TEST_LIST whose indices are listed in TESTNUMS.

  If we're running the tests in parallel spawn as much parallel processes
  as requested and gather the results in a temp. buffer when a child
  process is finished.
  """

  exit_code = 0
  finished_tests = []
  tests_started = 0

  if not parallel:
    for testnum in testnums:
      if run_one_test(testnum, test_list) == 1:
          exit_code = 1
  else:
    number_queue = Queue.Queue()
    for num in testnums:
      number_queue.put(num)

    threads = [ TestSpawningThread(number_queue) for i in range(parallel) ]
    for t in threads:
      t.start()

    for t in threads:
      t.join()

    # list of (index, result, stdout, stderr)
    results = []
    for t in threads:
      results += t.results
    results.sort()

    # terminate the line of dots
    print

    # all tests are finished, find out the result and print the logs.
    for (index, result, stdout_lines, stderr_lines) in results:
      if stdout_lines:
        for line in stdout_lines:
          sys.stdout.write(line)
      if stderr_lines:
        for line in stderr_lines:
          sys.stdout.write(line)
      if result == 1:
        exit_code = 1

  _cleanup_deferred_test_paths()
  return exit_code


def usage():
  prog_name = os.path.basename(sys.argv[0])
  print "%s [--url] [--fs-type] [--verbose|--quiet] [--parallel] \\" % \
        prog_name
  print "%s [--enable-sasl] [--cleanup] [--bin] [<test> ...]" \
      % (" " * len(prog_name))
  print "%s " % (" " * len(prog_name))
  print "%s [--list] [<test> ...]\n" % prog_name
  print "Arguments:"
  print " test          The number of the test to run (multiple okay), " \
        "or all tests\n"
  print "Options:"
  print " --list          Print test doc strings instead of running them"
  print " --fs-type       Subversion file system type (fsfs or bdb)"
  print " --http-library  DAV library to use (neon or serf)"
  print " --url           Base url to the repos (e.g. svn://localhost)"
  print " --verbose       Print binary command-lines (not with --quiet)"
  print " --quiet         Print only unexpected results (not with --verbose)"
  print " --cleanup       Whether to clean up"
  print " --enable-sasl   Whether to enable SASL authentication"
  print " --parallel      Run the tests in parallel"
  print " --bin           Use the svn binaries installed in this path"
  print " --use-jsvn      Use the jsvn (SVNKit based) binaries. Can be\n" \
        "                 combined with --bin to point to a specific path"
  print " --design        Test design mode: provides more detailed test\n" \
        "                 output and ignores all exceptions in the \n"  \
        "                 run_and_verify* functions. This option is only \n" \
        "                 useful during test development!"
  print " --help          This information"


# Main func.  This is the "entry point" that all the test scripts call
# to run their list of tests.
#
# This routine parses sys.argv to decide what to do.
def run_tests(test_list, serial_only = False):
  """Main routine to run all tests in TEST_LIST.

  NOTE: this function does not return. It does a sys.exit() with the
        appropriate exit code.
  """

  global test_area_url
  global pristine_url
  global fs_type
  global verbose_mode
  global quiet_mode
  global cleanup_mode
  global enable_sasl
  global is_child_process
  global svn_binary
  global svnadmin_binary
  global svnlook_binary
  global svnsync_binary
  global svnversion_binary
  global command_line_parsed
  global http_library
  global server_minor_version

  testnums = []
  # Should the tests be listed (as opposed to executed)?
  list_tests = False

  parallel = 0
  svn_bin = None
  use_jsvn = False

  try:
    opts, args = my_getopt(sys.argv[1:], 'vqhpc',
                           ['url=', 'fs-type=', 'verbose', 'quiet', 'cleanup',
                            'list', 'enable-sasl', 'help', 'parallel',
                            'bin=', 'http-library=', 'server-minor-version=', 
                            'use-jsvn', 'design'])
  except getopt.GetoptError, e:
    print "ERROR: %s\n" % e
    usage()
    sys.exit(1)

  for arg in args:
    if arg == "list":
      # This is an old deprecated variant of the "--list" option:
      list_tests = True
    elif arg.startswith('BASE_URL='):
      test_area_url = arg[9:]
    else:
      try:
        testnums.append(int(arg))
      except ValueError:
        print "ERROR:  invalid test number '%s'\n" % arg
        usage()
        sys.exit(1)

  for opt, val in opts:
    if opt == "--url":
      test_area_url = val

    elif opt == "--fs-type":
      fs_type = val

    elif opt == "-v" or opt == "--verbose":
      verbose_mode = True

    elif opt == "-q" or opt == "--quiet":
      quiet_mode = True

    elif opt == "--cleanup":
      cleanup_mode = True

    elif opt == "--list":
      list_tests = True

    elif opt == "--enable-sasl":
      enable_sasl = True

    elif opt == "-h" or opt == "--help":
      usage()
      sys.exit(0)

    elif opt == '-p' or opt == "--parallel":
      parallel = 5   # use 5 parallel threads.

    elif opt == '-c':
      is_child_process = True

    elif opt == '--bin':
      svn_bin = val

    elif opt == '--http-library':
      http_library = val

    elif opt == '--server-minor-version':
      server_minor_version = int(val)
      if server_minor_version < 4 or server_minor_version > 6:
        print "ERROR: test harness only supports server minor version 4 or 5"
        sys.exit(1)

    elif opt == '--use-jsvn':
      use_jsvn = True

    elif opt == '--design':
      setup_design_mode()

  if test_area_url[-1:] == '/': # Normalize url to have no trailing slash
    test_area_url = test_area_url[:-1]

  if verbose_mode and quiet_mode:
    sys.stderr.write("ERROR: 'verbose' and 'quiet' are incompatible\n")
    sys.exit(1)

  # Calculate pristine_url from test_area_url.
  pristine_url = test_area_url + '/' + pristine_dir
  if windows:
    pristine_url = pristine_url.replace('\\', '/')

  if use_jsvn:
    if svn_bin is None: 
      svn_bin = ''
    svn_binary = os.path.join(svn_bin, 'jsvn' + _bat)
    svnadmin_binary = os.path.join(svn_bin, 'jsvnadmin' + _bat)
    svnlook_binary = os.path.join(svn_bin, 'jsvnlook' + _bat)
    svnsync_binary = os.path.join(svn_bin, 'jsvnsync' + _bat)
    svnversion_binary = os.path.join(svn_bin, 'jsvnversion' + _bat)
    use_jsvn = False
  else:
    if svn_bin:
      svn_binary = os.path.join(svn_bin, 'svn' + _exe)
      svnadmin_binary = os.path.join(svn_bin, 'svnadmin' + _exe)
      svnlook_binary = os.path.join(svn_bin, 'svnlook' + _exe)
      svnsync_binary = os.path.join(svn_bin, 'svnsync' + _exe)
      svnversion_binary = os.path.join(svn_bin, 'svnversion' + _exe)

  command_line_parsed = True

  ######################################################################

  # Cleanup: if a previous run crashed or interrupted the python
  # interpreter, then `temp_dir' was never removed.  This can cause wonkiness.
  if not is_child_process:
    safe_rmtree(temp_dir)

  if not testnums:
    # If no test numbers were listed explicitly, include all of them:
    testnums = range(1, len(test_list))

  if list_tests:
    print "Test #  Mode   Test Description"
    print "------  -----  ----------------"
    for testnum in testnums:
      TestRunner(test_list[testnum], testnum).list()

    # done. just exit with success.
    sys.exit(0)

  # don't run tests in parallel when the tests don't support it or there
  # are only a few tests to run.
  if serial_only or len(testnums) < 2:
    parallel = 0

  # Setup the pristine repository
  actions.setup_pristine_repository()

  # Build out the default configuration directory
  create_config_dir(default_config_dir)

  # Run the tests.
  exit_code = _internal_run_tests(test_list, testnums, parallel)

  # Remove all scratchwork: the 'pristine' repository, greek tree, etc.
  # This ensures that an 'import' will happen the next time we run.
  if not is_child_process:
    safe_rmtree(temp_dir)

  # Cleanup after ourselves.
  _cleanup_deferred_test_paths()

  # Return the appropriate exit code from the tests.
  sys.exit(exit_code)

# the modules import each other, so we do this import very late, to ensure
# that the definitions in "main" have been completed.
import actions


### End of file.

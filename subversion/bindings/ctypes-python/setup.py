#!/usr/bin/env python

import errno, os, re, sys, tempfile

from distutils import log
from distutils.cmd import Command
from distutils.command.build import build as _build
from distutils.command.clean import clean as _clean
from distutils.core import setup
from distutils.dir_util import remove_tree
from distutils.errors import DistutilsExecError
from distutils.errors import DistutilsOptionError

from glob import glob
from tempfile import mkdtemp

class clean(_clean):
  """Special distutils command for cleaning the Subversion ctypes bindings."""
  description = "clean the Subversion ctypes Python bindings"

  def initialize_options (self):
    _clean.initialize_options(self)

  # initialize_options()

  def finalize_options (self):
    _clean.finalize_options(self)

  # finalize_options()

  def run(self):
    functions_py = os.path.join(os.path.dirname(__file__), "csvn", "core",
                                "functions.py")
    functions_pyc = os.path.join(os.path.dirname(__file__), "csvn", "core",
                                 "functions.pyc")

    if os.path.exists(functions_py):
      log.info("removing '%s'", os.path.normpath(functions_py))

      if not self.dry_run:
        os.remove(functions_py)
    else:
      log.warn("'%s' does not exist -- can't clean it",
               os.path.normpath(functions_py))

    if os.path.exists(functions_pyc):
      log.info("removing '%s'" % os.path.normpath(functions_pyc))

      if not self.dry_run:
        os.remove(functions_pyc)
    else:
      log.warn("'%s' does not exist -- can't clean it",
               os.path.normpath(functions_pyc))


    # Run standard clean command
    _clean.run(self)

  # run()

# class clean

class build(_build):
  """Special distutils command for building the Subversion ctypes bindings."""

  description = "build the Subversion ctypes Python bindings"

  _build.user_options.append(("apr=", None, "full path to where apr is "
                              "installed or the full path to the apr-config or "
                              "apr-1-config file"))
  _build.user_options.append(("apr-util=", None, "full path to where apr-util "
                              "is installed or the full path to the apu-config"
                              "apu-1-config file"))
  _build.user_options.append(("subversion=", None, "full path to where "
                              "Subversion is installed"))
  _build.user_options.append(("svn-headers=", None, "Full path to the "
                              "Subversion header files, if they are not in a "
                              "standard location"))
  _build.user_options.append(("ctypesgen=", None, "full path to where ctypesgen "
                              "is installed, to the ctypesgen source tree or "
                              "the full path to ctypesgen.py"))
  _build.user_options.append(("cppflags=", None, "extra flags to pass to the c "
                              "preprocessor"))
  _build.user_options.append(("ldflags=", None, "extra flags to pass to the "
                              "ctypesgen linker"))
  _build.user_options.append(("lib-dirs=", None, "colon-delimited list of paths "
                              "to append to the search path"))
  _build.user_options.append(("save-preprocessed-headers=", None, "full path to "
                              "where to save the preprocessed headers"))

  def initialize_options (self):
    _build.initialize_options(self)
    self.apr = None
    self.apr_util = None
    self.ctypesgen = None
    self.subversion = None
    self.svn_headers = None
    self.cppflags = ""
    self.ldflags = ""
    self.lib_dirs = None
    self.save_preprocessed_headers = None

  # initialize_options()

  def finalize_options (self):
    _build.finalize_options(self)

    # Distutils doesn't appear to like when you have --dry-run after the build
    # build command so fail out if this is the case.
    if self.dry_run != self.distribution.dry_run:
      raise DistutilsOptionError("The --dry-run flag must be specified " \
                                 "before the 'build' command")

  # finalize_options()

  ##############################################################################
  # Get the APR configuration
  ##############################################################################
  def get_apr_config (self):
    flags = []
    ldflags = []
    library_path = []
    ferr = None
    apr_include_dir = None
    
    fout = self.run_cmd("%s --includes --cppflags" % self.apr_config)
    if fout:
      flags = fout.split()
      apr_prefix = self.run_cmd("%s --prefix" % self.apr_config)
      apr_prefix = apr_prefix.strip()
      apr_include_dir = self.run_cmd("%s --includedir" % self.apr_config).strip()
      apr_version = self.run_cmd("%s --version" % self.apr_config).strip()
      cpp  = self.run_cmd("%s --cpp" % self.apr_config).strip()
      fout = self.run_cmd("%s --ldflags --link-ld" % self.apr_config)
      if fout:
        ldflags = fout.split()
      else:
        log.error(ferr)
        raise DistutilsExecError("Problem running '%s'.  Check the output " \
                                 "for details" % self.apr_config)
          
      fout = self.run_cmd("%s --includes" % self.apu_config)
      if fout:
        flags += fout.split()
        fout = self.run_cmd("%s --ldflags --link-ld" % self.apu_config)
        if fout:
          ldflags += fout.split()
        else:
          log.error(ferr)
          raise DistutilsExecError("Problem running '%s'.  Check the output " \
                                   "for details" % self.apr_config)
          
        subversion_prefixes = [
          self.subversion,
          "/usr/local",
          "/usr"
          ]
        
        if self.subversion != "/usr":
          ldflags.append("-L%s/lib" % self.subversion)
        flags.append("-I%s" % self.svn_include_dir)

        # List the libraries in the order they should be loaded
        libraries = [ 
          "svn_subr-1",
          "svn_diff-1",
          "svn_delta-1",
          "svn_fs-1",
          "svn_repos-1",
          "svn_wc-1",
          "svn_ra-1",
          "svn_client-1",
          ]
          
        for lib in libraries:
          ldflags.append("-l%s" % lib)
          
        if apr_prefix != '/usr':
          library_path.append("%s/lib" % apr_prefix)
          if self.subversion != '/usr' and self.subversion != apr_prefix:
            library_path.append("%s/lib" % self.subversion)

        return (apr_prefix, apr_include_dir, cpp + " " + self.cppflags,
                " ".join(ldflags) + " " + self.ldflags, " ".join(flags),
                ":".join(library_path))
  
  # get_apr_config()

  ##############################################################################
  # Build csvn/core/functions.py
  ##############################################################################
  def build_functions_py(self):
    (apr_prefix, apr_include_dir, cpp, ldflags, flags,
     library_path) = self.get_apr_config()
    tempdir = mkdtemp()
    try:
      includes = ('%s/svn_*.h '
                  '%s/ap[ru]_*.h' % (self.svn_include_dir, apr_include_dir))
      cmd = ["cd %s && %s %s --cpp '%s %s' %s "
             "%s -o svn_all.py --no-macro-warnings" % (tempdir, sys.executable,
                                                       self.ctypesgen_py, cpp,
                                                       flags, ldflags,
                                                       includes)]
      if self.lib_dirs:
        cmd.extend('-R ' + x for x in self.lib_dirs.split(":"))
      cmd = ' '.join(cmd)

      if self.save_preprocessed_headers:
        cmd += " --save-preprocessed-headers=%s" % \
            os.path.abspath(self.save_preprocessed_headers)

      if self.verbose or self.dry_run:
        status = self.execute(os.system, (cmd,), cmd)
      else:
        f = os.popen(cmd, 'r')
        f.read() # Required to avoide the 'Broken pipe' error.
        status = f.close() # None is returned for the usual 0 return code
      
      if os.name == "posix" and status and status != 0:
        if os.WIFEXITED(status):
          status = os.WEXITSTATUS(status)
          if status != 0:
            sys.exit(status)
          elif os.WIFSIGNALED(status):
            log.error("ctypesgen.py killed with signal %d" % os.WTERMSIG(status))
            sys.exit(2)
          elif os.WIFSTOPPED(status):
            log.error("ctypesgen.py stopped with signal %d" % os.WSTOPSIG(status))
            sys.exit(2)
          else:
            log.error("ctypesgen.py exited with invalid status %d", status)
            sys.exit(2)

      if not self.dry_run:
        out = file("%s/svn_all2.py" % tempdir, "w")
        for line in file("%s/svn_all.py" % tempdir):
          line = line.replace("restype = POINTER(svn_error_t)",
                              "restype = SVN_ERR")

          if not line.startswith("FILE ="):
            out.write(line)
        out.close()

      cmd = ("cat csvn/core/functions.py.in %s/svn_all2.py "
             "> csvn/core/functions.py" % tempdir)
      self.execute(os.system, (cmd,), cmd)

      log.info("Generated csvn/core/functions.py successfully")

    finally:
      # Remove temporary directory
      remove_tree(tempdir)

  # build_functions_py()

  def run_cmd(self, cmd):
    return os.popen(cmd).read()
  
  # run_cmd()

  def validate_options(self):
    # Validate apr
    if not self.apr:
      raise DistutilsOptionError("The --apr option is mandatory and " \
                                 "must point to a valid apr installation " \
                                 "or to either the apr-config file or the " \
                                 "apr-1-config file")

    if os.path.exists(self.apr):
      if os.path.isdir(self.apr):
        if os.path.exists(os.path.join(self.apr, "bin", "apr-config")):
          self.apr_config = os.path.join(self.apr, "bin", "apr-config")
        elif os.path.exists(os.path.join(self.apr, "bin", "apr-1-config")):
          self.apr_config = os.path.join(self.apr, "bin", "apr-1-config")
        else:
          self.apr_config = None
      elif os.path.basename(self.apr) in ("apr-config", "apr-1-config"):
        self.apr_config = self.apr
      else:
        self.apr_config = None
    else:
      self.apr_config = None

    if not self.apr_config:
      raise DistutilsOptionError("The --apr option is not valid.  It must " \
                                 "point to a valid apr installation or " \
                                 "to either the apr-config file or the " \
                                 "apr-1-config file")

    # Validate apr-util
    if not self.apr_util:
      raise DistutilsOptionError("The --apr-util option is mandatory and " \
                                 "must point to a valid apr-util " \
                                 "installation or to either the apu-config " \
                                 "file or the apu-1-config file")

    if os.path.exists(self.apr_util):
      if os.path.isdir(self.apr_util):
        if os.path.exists(os.path.join(self.apr_util, "bin", "apu-config")):
          self.apu_config = os.path.join(self.apr_util, "bin", "apu-config")
        elif os.path.exists(os.path.join(self.apr_util, "bin", "apu-1-config")):
          self.apu_config = os.path.join(self.apr_util, "bin", "apu-1-config")
        else:
          self.apu_config = None
      elif os.path.basename(self.apr_util) in ("apu-config", "apu-1-config"):
        self.apu_config = self.apr_util
      else:
        self.apu_config = None
    else:
      self.apu_config = None

    if not self.apu_config:
      raise DistutilsOptionError("The --apr-util option is not valid.  It " \
                                 "must point to a valid apr-util " \
                                 "installation or to either the apu-config " \
                                 "file or the apu-1-config file")

    # Validate subversion
    if not self.subversion:
      raise DistutilsOptionError("The --subversion option is mandatory and " \
                                 "must point to a valid Subversion " \
                                 "installation")

    # Validate svn-headers, if present
    if self.svn_headers:
      if os.path.isdir(self.svn_headers):
        if os.path.exists(os.path.join(self.svn_headers, "svn_client.h")):
          self.svn_include_dir = self.svn_headers
        elif os.path.exists(os.path.join(self.svn_headers, "subversion-1",
                                         "svn_client.h")):
          self.svn_include_dir = os.path.join(self.svn_headers, "subversion-1")
        else:
          self.svn_include_dir = None
      else:
        self.svn_include_dir = None
    elif os.path.exists(os.path.join(self.subversion, "include",
                                     "subversion-1")):
      self.svn_include_dir = "%s/include/subversion-1" % self.subversion
    else:
      self.svn_include_dir = None

    if not self.svn_include_dir:
      msg = ""

      if self.svn_headers:
        msg = "The --svn-headers options is not valid.  It must point to " \
              "either a Subversion include directory or the Subversion " \
              "include/subversion-1 directory."
      else:
        msg = "The --subversion option is not valid. " \
              "Could not locate %s/include/" \
              "subversion-1/svn_client.h" % self.subversion

      raise DistutilsOptionError(msg)

    # Validate ctypesgen
    if not self.ctypesgen:
      raise DistutilsOptionError("The --ctypesgen option is mandatory and " \
                                 "must point to a valid ctypesgen " \
                                 "installation")

    if os.path.exists(self.ctypesgen):
      if os.path.isdir(self.ctypesgen):
        if os.path.exists(os.path.join(self.ctypesgen, "ctypesgen.py")):
          self.ctypesgen_py = os.path.join(self.ctypesgen, "ctypesgen.py")
        elif os.path.exists(os.path.join(self.ctypesgen, "bin",
                                         "ctypesgen.py")):
          self.ctypesgen_py = os.path.join(self.ctypesgen, "bin",
                                           "ctypesgen.py")
        else:
          self.ctypesgen_py = None
      elif os.path.basename(self.ctypesgen) == "ctypesgen.py":
          self.ctypesgen_py = self.ctypesgen
      else:
        self.ctypesgen_py = None
    else:
      self.ctypesgen_py = None

    if not self.ctypesgen_py:
      raise DistutilsOptionError("The --ctypesgen option is not valid.  It " \
                                 "must point to a valid ctypesgen " \
                                 "installation, a ctypesgen source tree or " \
                                 "to the ctypesgen.py script")

  # validate_functions()

  def run (self):
    # We only want to build if functions.py is not present.
    if not os.path.exists(os.path.join(os.path.dirname(__file__),
                                       "csvn", "core",
                                       "functions.py")):
      if 'build' not in self.distribution.commands:
        raise DistutilsOptionError("You must run 'build' explicitly before " \
                                   "you can proceed")

      # Validate the command line options
      self.validate_options()

      # Generate functions.py
      self.build_functions_py()
    else:
      log.info("csvn/core/functions.py was not regenerated (output up-to-date)")

    # Run the standard build command.
    _build.run(self)

  # run()

# class build

setup(cmdclass={'build': build, 'clean': clean},
      name='svn-ctypes-python-bindings',
      version='0.1',
      description='Python bindings for the Subversion version control system.',
      author='The Subversion Team',
      author_email='dev@subversion.tigris.org',
      url='http://subversion.tigris.org',
      packages=['csvn', 'csvn.core', 'csvn.ext'],
     )

# TODO: We need to create our own bdist_rpm implementation so that we can pass
#       our required arguments to the build command being called by bdist_rpm.

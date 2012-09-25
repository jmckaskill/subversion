#!/usr/bin/env python
#
#
# NOTICE: THIS SCRIPT IS OBSOLETE/DEPRECATED
#
#
# setup.py:  Distutils-based config/build/install for the Python bindings
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2003 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import os
import sys
import getopt


print """
----------------------------------------------------------------------------

WARNING WARNING -- THIS SCRIPT IS NOW OBSOLETE

To build the Python bindings, you should use 'make swig-py' at the
top-level. To install them, you should 'make install-swig-py'.

For Windows ... ???

----------------------------------------------------------------------------
"""


def _do_usage():
  print "Usage: setup.py [OPTIONS] build"
  print "       setup.py install [--prefix PREFIX]"
  print "       setup.py install_lib [--install-dir DIR]"
  print ""
  print "Options:"
  print "   -I dir      " + \
        "search DIR for includes (multiple instances allowed)"
  print "   -L dir      " + \
        "search DIR for libraries (multiple instances allowed)"
  print "   -C option   " + \
        "pass OPTION to the compiler at compile time (multiple instances " + \
        "allowed)"
  print "   -R option   " + \
        "pass OPTION to the compiler at link time (multiple instances " + \
        "allowed)"
  print "   -S dir      " + \
        "the DIR for the source of the subversion swig bindings"
  print "   -s path     " + \
        "use the swig binary found at PATH"
  sys.exit(0)

# Default option values
include_dirs = []
library_dirs = []
extra_compile_args = []
extra_link_args = []
source_dir = '..'
swig_location = None

# No args?  Give usage.
if len(sys.argv) < 2:
  _do_usage()

# Parse the command-line arguments, keeping what we want and letting
# distutils have the rest.  Distutils parameters should come after
# the target as in 'python setup.py build --prefix=/usr/local' and
# parameters for us should appear before the target as in
# 'python setup.py -I/usr/include build'.
options, leftovers = getopt.getopt(sys.argv[1:], "I:L:C:R:S:s:h",
                                   ["help"])
for option in options:
  if option[0] == '-I':
    include_dirs.append(option[1])
  if option[0] == '-L':
    library_dirs.append(option[1])
  if option[0] == '-C':
    extra_compile_args.append(option[1])
  if option[0] == '-R':
    extra_link_args.append(option[1])
  if option[0] == '-S':
    source_dir = option[1]
  if option[0] == '-s':
    swig_location = option[1]
  if option[0] == '-h':
    _do_usage()

  if option[0] == '--help':
    _do_usage()

  # All long options just get passed through
  if option[0][:2] == '--':
    leftovers.append(option[0])
    leftovers.append(option[1])
sys.argv[1:] = leftovers

from distutils import core
from distutils.command import build_ext
from distutils import dir_util

class build_swig(build_ext.build_ext):
  def initialize_options(self):
    build_ext.build_ext.initialize_options(self)
    self.build_base = None
    
  def finalize_options(self):
    build_ext.build_ext.finalize_options(self)
    self.set_undefined_options('build',
                               ('build_base', 'build_base'),
                               )

  def find_swig(self):
    global swig_location
    if swig_location is None:
      return build_ext.build_ext.find_swig(self)
    else:
      return swig_location

  def swig_sources(self, sources):
    swig = self.find_swig()
    swig_cmd = [swig, "-c", "-python", "-noproxy"]
    for dir in self.include_dirs:
      swig_cmd.append("-I" + dir)

    dir_util.mkpath(self.build_base, 0777, self.verbose, self.dry_run)

    new_sources = [ ]
    for source in sources:
      target = os.path.join(self.build_base,
                            os.path.basename(source[:-2]) + ".c")
      self.announce("swigging %s to %s" % (source, target))
      self.spawn(swig_cmd + ["-o", target, source])
      new_sources.append(target)

    return new_sources


core.setup(name="Subversion",
           version="0.0.0",
           description="bindings for Subversion libraries",
           author_email="dev@subversion.tigris.org",
           url="http://subversion.tigris.org/",

           packages=['svn'],

           package_dir={"":source_dir+'/python'},
           include_dirs=include_dirs,

           ext_package="svn",
           ext_modules=[
             core.Extension("_client",
                            [source_dir + "/svn_client.i"],
                            libraries=['svn_client-1', 'svn_swig_py-1',
                                       'swigpy'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),
             core.Extension("_delta",
                            [source_dir + "/svn_delta.i"],
                            libraries=['svn_delta-1', 'svn_swig_py-1',
                                       'swigpy'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),
             core.Extension("_fs",
                            [source_dir + "/svn_fs.i"],
                            libraries=['svn_fs-1', 'svn_swig_py-1', 'swigpy'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),
             core.Extension("_ra",
                            [source_dir + "/svn_ra.i"],
                            libraries=['svn_repos-1', 'svn_swig_py-1',
                                       'svn_ra-1', 'swigpy'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),
             core.Extension("_repos",
                            [source_dir + "/svn_repos.i"],
                            libraries=['svn_repos-1', 'svn_swig_py-1',
                                       'swigpy'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),
             core.Extension("_wc",
                            [source_dir + "/svn_wc.i"],
                            libraries=['svn_wc-1', 'svn_swig_py-1', 'swigpy'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),
             core.Extension("_util",
                            [source_dir + "/util.i"],
                            libraries=['svn_subr-1', 'svn_swig_py-1', 'swigpy', 'apr-0'],
                            library_dirs=library_dirs,
                            extra_compile_args=extra_compile_args,
                            extra_link_args=extra_link_args,
                            ),

             ### will 'auth' be its own, or bundled elsewhere?
             #core.Extension("_auth",
             #               ["../svn_auth.i"],
             #               libraries=['svn_subr-1', 'swigpy', 'apr-0'],
             #               library_dirs=library_dirs,
             #               ),
             ],

           cmdclass={'build_ext' : build_swig},
           )
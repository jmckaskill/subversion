#!/usr/bin/env python
#
#  svneditor.py: a mock $SVN_EDITOR for the Subversion test suite
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2006 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import sys
import os

def main():
    if len(sys.argv) != 2:
        print "usage: svneditor.py file"
        sys.exit(1)

    filename = sys.argv[1]

    # Read in the input file.
    f = open(filename)
    contents = f.read()
    f.close()
    
    funcname = os.environ['SVNTEST_EDITOR_FUNC']
    func = sys.modules['__main__'].__dict__[funcname]

    # Run the conversion.
    contents = func(contents)

    # Write edited version back to the file.
    f = open(filename, 'w')
    f.write(contents)
    f.close()

def foo_to_bar(m):
    return m.replace('foo', 'bar')

main()
sys.exit(0)

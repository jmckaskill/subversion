#!/usr/bin/env python2
#
# Usage: scramble-tree.py <dir> [SEED]
#
# Makes multiple random file changes to a directory tree, for testing.
#
# This script will add some new files, remove some existing files, add
# text to some existing files, and delete text from some existing
# files.  It will also leave some files completely untouched.
#
# The exact set of changes made is always the same for identical trees,
# where "identical" means the names of files and directories are the
# same, and they are arranged in the same tree structure (the actual
# contents of files may differ).  If two are not identical, the sets of
# changes scramble-tree.py will make may differ arbitrarily.
#
# Directories named .svn/ and CVS/ are ignored.
#
# Example scenario, starting with a pristine Subversion working copy:
#
#   $ ls
#   foo/
#   $ svn st foo
#   $ cp -r foo bar
#   $ svn st bar
#   $ scramble-tree.py foo
#   $ svn st foo
#   [... see lots of scary status output ...]
#   $ scramble-tree.py bar
#   [... see the exact same scary status output ...]
#   $ scramble-tree.py foo
#   [... see a new bunch of scary status output ...]
#   $

import os
import sys
import random
import md5
import base64

class hashDir:

  """Given a directory, creates a string containing all directories
  and files under that directory (sorted alphanumerically) and makes a
  base64-encoded md5 hash of the resulting string.  Call
  hashDir.gen_seed() to generate a seed value for this tree."""

  def __init__(self, rootdir):
    self.allfiles = []
    os.path.walk(rootdir, self.walker_callback, len(rootdir))


  def gen_seed(self):
    # Return a base64-encoded (kinda ... strip the '==\n' from the
    # end) MD5 hash of sorted tree listing.
    self.allfiles.sort()
    return base64.encodestring(md5.md5(''.join(self.allfiles)).digest())[:-3]

    
  def walker_callback(self, baselen, dirname, fnames):
    if ((dirname == '.svn')
        or (dirname == 'CVS')):
      return

    self.allfiles.append(dirname[baselen:])

    for filename in fnames:
      path = os.path.join(dirname, filename)
      if not os.path.isdir(path):
        self.allfiles.append(path[baselen:])



class Scrambler:
  def __init__(self, seed):
    self.greeking = """
======================================================================
This is some text that was inserted into this file by the lovely and
talented scramble-tree.py script.
======================================================================
"""
    self.file_modders = [self.append_to_file,
                         self.append_to_file,
                         self.append_to_file,                         
                         self.remove_from_file,
                         self.remove_from_file,
                         self.remove_from_file,
                         self.delete_file,
                         ]
    self.rand = random.Random(seed)


  def shrink_list(self, list):
    # remove 5 random lines
    if len(list) < 6:
      return list
    for i in range(5): # TODO remove a random number of lines in a range
      j = self.rand.randrange(len(list) - 1)
      del list[j]
    return list


  def append_to_file(self):
    print 'append_to_file:', self.path
    fh = open(self.path, "a")
    fh.write(self.greeking)
    fh.close()


  def remove_from_file(self):
    print 'remove_from_file:', self.path
    lines = self.shrink_list(open(self.path, "r").readlines())
    open(self.path, "w").writelines(lines)


  def delete_file(self):
    print 'delete_file:', self.path
    os.remove(self.path)


  def munge_file(self, path):
    self.path = path
    # Only do something 33% of the time
    if self.rand.randrange(3):
      self.rand.choice(self.file_modders)()


  def maybe_add_file(self, dir):
    if self.rand.randrange(3) == 2:
      path = os.path.join(dir, 'newfile.txt')
      print "maybe_add_file:", path
      open(path, 'w').write(self.greeking)



def usage():
  print "Usage:", sys.argv[0], "<directory> [SEED]"
  sys.exit(255)


def walker_callback(scrambler, dirname, fnames):
  if ((dirname.find('.svn') != -1)
      or dirname.find('CVS') != -1):
    return

  scrambler.maybe_add_file(dirname)
  for filename in fnames:
    path = os.path.join(dirname, filename)
    if not os.path.isdir(path):
      scrambler.munge_file(path)

if __name__ == '__main__':
  # If we have no ARG, exit
  argc = len(sys.argv)
  if argc < 2 or argc > 3:
    usage()
    sys.exit(254)

  rootdir = sys.argv[1]
  if argc == 2:
    seed = hashDir(rootdir).gen_seed()
  else:
    seed = sys.argv[2]
  print 'Using seed: ' + seed
  scrambler = Scrambler(seed)
  
  # Fire up the treewalker
  os.path.walk(rootdir, walker_callback, scrambler)


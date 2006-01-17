#!/usr/bin/env python
#
# USAGE: blame.py [-r REV] repos-path file
#

import sys
import os
import getopt
import difflib
from svn import fs, core, repos

CHUNK_SIZE = 100000

def blame(path, filename, rev=None):
  
  annotresult = {}
  if path[-1] == "/":
     path = path[:-1]

  repos_ptr = repos.open(path)
  fsob = repos.fs(repos_ptr)
 
  if rev is None:
    rev = fs.youngest_rev(fsob)
  filedata = '' 
  for i in xrange(0, rev+1):
    root = fs.revision_root(fsob, i)
    if fs.check_path(root, filename) != core.svn_node_none:
      first = i
      break
  print "First revision is %d" % first
  print "Last revision is %d" % rev
  for i in xrange(first, rev+1):
    previousroot = root
    root = fs.revision_root(fsob, i)
    if i != first:
      if not fs.contents_changed(root, filename, previousroot, filename):
        continue
      
    file = fs.file_contents(root, filename)
    previousdata = filedata
    filedata = ''
    while 1:
      data = core.svn_stream_read(file, CHUNK_SIZE)
      if not data:
        break
      filedata = filedata + data

    print "Current revision is %d" % i
    diffresult = difflib.ndiff(previousdata.splitlines(1),
                               filedata.splitlines(1))
    #    print ''.join(diffresult)
    k = 0    
    for j in diffresult:
      if j[0] == ' ':
        if annotresult.has_key (k):
          k = k + 1
          continue
        else:
	  annotresult[k] = (i, j[2:])
          k = k + 1
          continue
      elif j[0] == '?':
        continue
      annotresult[k] = (i, j[2:])
      if j[0] != '-':
        k = k + 1
#    print ''.join(diffresult)
#  print annotresult 
  for x in xrange(len(annotresult.keys())):
     sys.stdout.write("Line %d (rev %d):%s" % (x,
                                               annotresult[x][0],
                                               annotresult[x][1]))

def usage():
  print "USAGE: blame.py [-r REV] repos-path file"
  sys.exit(1)

def main():
  opts, args = getopt.getopt(sys.argv[1:], 'r:')
  if len(args) != 2:
    usage()
  rev = None
  for name, value in opts:
    if name == '-r':
      rev = int(value)
  blame(args[0], args[1], rev)

if __name__ == '__main__':
  main()

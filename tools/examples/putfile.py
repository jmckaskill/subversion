#!/usr/bin/env python2
#
# USAGE: putfile.py [-m commitmsg] [-u username] file repos-path 
#
# put a file into an SVN repository
#

import sys
import os
import getopt

from svn import fs, core, repos, delta

def putfile(pool, fname, rpath, uname="", commitmsg=""):
  if rpath[-1] == "/":
     rpath = rpath[:-1]

  repos_ptr = repos.svn_repos_open(rpath, pool)
  fsob = repos.svn_repos_fs(repos_ptr)

  # open a transaction against HEAD
  rev = fs.youngest_rev(fsob, pool)

  txn = repos.svn_repos_fs_begin_txn_for_commit(repos_ptr, rev, uname, commitmsg, pool)

  root = fs.txn_root(txn, pool)
  rev_root = fs.revision_root(fsob, rev, pool)

  kind = fs.check_path(root, fname, pool)
  if kind == core.svn_node_none:
    print "file '%s' does not exist, creating..." % fname
    fs.make_file(root, fname, pool)
  elif kind == core.svn_node_dir:
    print "File '%s' is a dir." % fname
    return 
  else:
    print "Updating file '%s'" % fname

  handler, baton = fs.apply_textdelta(root, fname, None, None, pool)

  ### it would be nice to get an svn_stream_t. for now, just load in the
  ### whole file and shove it into the FS.
  delta.svn_txdelta_send_string(open(fname, 'rb').read(),
                                handler, baton,
                                pool)

  newrev = repos.svn_repos_fs_commit_txn(repos_ptr, txn, pool)
  print "revision: ", newrev

def usage():
  print "USAGE: putfile.py [-m commitmsg] [-u username] file repos-path"
  sys.exit(1)

def main():
  opts, args = getopt.getopt(sys.argv[1:], 'm:u:')
  if len(args) != 2:
    usage()

  uname = commitmsg = ""

  for name, value in opts:
    if name == '-u':
      uname = value
    if name == '-m':
      commitmsg = value
  core.run_app(putfile, args[0], args[1], uname, commitmsg)

if __name__ == '__main__':
  main()

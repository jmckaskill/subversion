#!/usr/bin/env python
#
#  info_tests.py:  testing the svn info command
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2000-2008 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# See basic-tests.py for more svn info tests.

# General modules
import shutil, stat, re, os

# Our testing module
import svntest
from svntest import wc
from svntest.tree import SVNTreeError, SVNTreeUnequal, \
                         SVNTreeIsNotDirectory, SVNTypeMismatch

# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = wc.StateItem

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.

#----------------------------------------------------------------------

# Helpers for text output.
def verify_lines(lines, regexes):
  """Verify that each of the given regular expressions matches exactly
     one line in the list of lines."""
  for regex in regexes:
    found = 0
    for line in lines:
      if re.search(regex, line):
        if found == 1:
          print "Pattern '%s' found a second time." % regex
          print "Line: %s" % line
          raise SVNTreeUnequal
        lines.remove(line)
        found = 1
    if found == 0:
      print "Pattern '%s' not found." % regex
      raise SVNTreeUnequal

# Helpers for XML output
def verify_xml_elements(lines, exprs):
  """Verify that each of the given expressions matches exactly one XML
     element in the list of lines. Each expression is a tuple containing
     a name (a string), a set of attribute name-value pairs (a dict of
     string->string), and element content (a string).  The attribute dict
     and the content string are by default empty.

     Expression format: [ ('name', {'att': 'val', ...}, 'text') , ...]

     Limitations:
     We don't verify that the input is a valid XML document.
     We can't verify text mixed with child elements.
     We don't handle XML comments.
     All of these are taken care of by the Relax NG schemas.
  """
  xml_version_re = re.compile(r"<\?xml\s+[^?]+\?>")

  str = ""
  for line in lines:
    str += line
  m = xml_version_re.match(str)
  if m:
    str = str[m.end():] # skip xml version tag
  (unmatched_str, unmatched_exprs) = match_xml_element(str, exprs)
  if unmatched_exprs:
    print "Failed to find the following expressions:"
    for expr in unmatched_exprs:
      print expr
    raise SVNTreeUnequal

def match_xml_element(str, exprs):
  """Read from STR until the start of an element. If no element is found,
     return the arguments.  Get the element name, attributes and text content.
     If not empty, call recursively on the text content.  Compare the current
     element to all expressions in EXPRS.  If no elements were found in the
     current element's text, include the text in the comparison (i.e., we
     don't support mixed content).  Return the unmatched part of the string
     and any unmatched expressions.
  """
  start_tag_re = re.compile(r"[^<]*<(?P<name>[\w-]+)", re.M)
  atttribute_re = re.compile(
                 r"\s+(?P<key>[\w-]+)\s*=\s*(['\"])(?P<val>[^'\"]*)\2", re.M)
  self_closing_re = re.compile(r"\s*/>", re.M)
  content_re_str = "\\s*>(?P<content>.*?)</%s\s*>"

  m = start_tag_re.match(str)
  if not m:
    return (str, exprs)
  name = m.group('name')
  str = str[m.end():]
  atts = {}
  while 1:
    m = atttribute_re.match(str)
    if not m:
      break
    else:
      atts[m.group('key')] = m.group('val')
      str = str[m.end():]
  m = self_closing_re.match(str)
  if m:
    content = ''
    str = str[m.end():]
  else:
    content_re = re.compile(content_re_str % name, re.DOTALL)
    m = content_re.match(str)
    if not m:
      print "No XML end-tag for '%s' found in '%s...'" % (name, str[:100])
      raise(SVNTreeUnequal)
    content = m.group('content')
    str = str[m.end():]
  if content != '':
    while 1:
      (new_content, exprs) = match_xml_element(content, exprs)
      if new_content == content:
        # there are no (more) child elements
        break
      else:
        content = new_content
  if exprs:
    for expr in exprs:
      # compare element names
      e_name = expr[0]
      if (e_name != name):
        continue
      # compare element attributes
      e_atts = {}
      if len(expr) > 1:
        e_atts = expr[1]
      if not same_dict(e_atts, atts):
        continue
      # compare element content (text only)
      e_content = ''
      if len(expr) > 2:
        e_content = expr[2]
      if (not re.search(e_content, content)):
        continue
      # success!
      exprs.remove(expr)
  return (str, exprs)

def same_dict(d1, d2):
  "Helper function to test if 2 Python dictionaries have the same content"
    # Might be simpler to use sets, but they first appear in Python 2.3,
    # darn it...
  if len(d1) != len(d2):
    return False
  for (key, val) in d1.items():
    if (key not in d2 or d2[key] != val):
      return False
  return True

def info_with_tree_conflicts(sbox):
  "info with tree conflicts"

  # Info messages reflecting tree conflict status.
  # These tests correspond to use cases 1-3 in
  # notes/tree-conflicts/use-cases.txt.

  svntest.actions.build_greek_tree_conflicts(sbox)
  wc_dir = sbox.wc_dir
  G = os.path.join(wc_dir, 'A', 'D', 'G')

  scenarios = [
    # (filename, action_verb, action, reason)
    ('pi',  'edit',   'edited',  'deleted'),
    ('rho', 'delete', 'deleted', 'edited'),
    ('tau', 'delete', 'deleted', 'deleted'),
    ]

  for fname, action_verb, action, reason in scenarios:
    path = os.path.join(G, fname)

    # check plain info
    exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                  [], 'info',
                                                                  path)
    expected_str1 = "The update attempted to " + action_verb + " '" + fname
    verify_lines(output, [expected_str1])

    # check XML info
    exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                  [], 'info',
                                                                  path,
                                                                  '--xml')

    # In the XML, action and reason are past tense: 'edited' not 'edit'.
    verify_xml_elements(output,
                        [('tree-conflict', {'victim'   : fname,
                                            'kind'     : 'file',
                                            'operation': 'update',
                                            'action'   : action,
                                            'reason'   : reason,
                                            },
                          )])

def info_on_added_file(sbox):
  """info on added file"""
  
  svntest.actions.make_repo_and_wc(sbox)
  wc_dir = sbox.wc_dir
  
  # create new file
  new_file = os.path.join(wc_dir, 'new_file')
  svntest.main.file_append(new_file, '')

  svntest.main.run_svn(None, 'add', new_file)
  
  exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                [], 'info',
                                                                new_file)

  uuid_regex = '[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}'

  # check that we have a Repository Root and Repository UUID
  expected = ['Path: %s\n' % new_file.replace('\\', '\\\\'),
              'Name: new_file',
              'URL: .*/new_file',
              'Repository Root: .*',
              'Revision: 0',
              'Node Kind: file',
              'Schedule: add',
              'Repository UUID: %s' % uuid_regex]

  verify_lines(output, expected)
  
  # check XML info
  exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                [], 'info',
                                                                new_file,
                                                                '--xml')

  verify_xml_elements(output,
                      [('entry',    {'kind'     : 'file',
                                     'path'     : new_file,
                                     'revision' : '0'}),
                       ('url',      {}, '.*/new_file'),
                       ('root',     {}, '.*'),
                       ('uuid',     {}, uuid_regex),
                       ('depth',    {}, 'infinity'),
                       ('schedule', {}, 'add')])

def info_on_mkdir(sbox):
  """info on new dir with mkdir"""
  svntest.actions.make_repo_and_wc(sbox)
  wc_dir = sbox.wc_dir
  
  # create a new directory using svn mkdir
  new_dir = os.path.join(wc_dir, 'new_dir')
  svntest.main.run_svn(None, 'mkdir', new_dir)
  
  exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                [], 'info',
                                                                new_dir)

  uuid_regex = '[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}'

  # check that we have a Repository Root and Repository UUID
  expected = ['Path: %s\n' % new_dir.replace('\\', '\\\\'),
              'URL: .*/new_dir',
              'Repository Root: .*',
              'Revision: 0',
              'Node Kind: directory',
              'Schedule: add',
              'Repository UUID: %s' % uuid_regex]

  verify_lines(output, expected)
  
  # check XML info
  exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                [], 'info',
                                                                new_dir,
                                                                '--xml')
  print 'output: %s \n\n' % output
  verify_xml_elements(output,
                      [('entry',    {'kind'     : 'dir',
                                     'path'     : new_dir,
                                     'revision' : '0'}),
                       ('url',      {}, '.*/new_dir'),
                       ('root',     {}, '.*'),
                       ('uuid',     {}, uuid_regex),
                       ('depth',    {}, 'infinity'),
                       ('schedule', {}, 'add')])

########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              info_with_tree_conflicts,
              XFail(info_on_added_file),
              XFail(info_on_mkdir)
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

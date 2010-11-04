#!/usr/bin/env python
#
#  info_tests.py:  testing the svn info command
#
#  Subversion is a tool for revision control.
#  See http://subversion.apache.org for more information.
#
# ====================================================================
#    Licensed to the Apache Software Foundation (ASF) under one
#    or more contributor license agreements.  See the NOTICE file
#    distributed with this work for additional information
#    regarding copyright ownership.  The ASF licenses this file
#    to you under the Apache License, Version 2.0 (the
#    "License"); you may not use this file except in compliance
#    with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing,
#    software distributed under the License is distributed on an
#    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#    KIND, either express or implied.  See the License for the
#    specific language governing permissions and limitations
#    under the License.
######################################################################

# See basic-tests.py for more svn info tests.

# General modules
import shutil, stat, re, os

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
    print("Failed to find the following expressions:")
    for expr in unmatched_exprs:
      print(expr)
    raise svntest.tree.SVNTreeUnequal

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
  while True:
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
      print("No XML end-tag for '%s' found in '%s...'" % (name, str[:100]))
      raise(svntest.tree.SVNTreeUnequal)
    content = m.group('content')
    str = str[m.end():]
  if content != '':
    while True:
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
      if e_atts != atts:
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

def info_with_tree_conflicts(sbox):
  "info with tree conflicts"

  # Info messages reflecting tree conflict status.
  # These tests correspond to use cases 1-3 in
  # notes/tree-conflicts/use-cases.txt.

  svntest.actions.build_greek_tree_conflicts(sbox)
  wc_dir = sbox.wc_dir
  G = os.path.join(wc_dir, 'A', 'D', 'G')

  scenarios = [
    # (filename, action, reason)
    ('pi',  'edit',   'delete'),
    ('rho', 'delete', 'edit'),
    ('tau', 'delete', 'delete'),
    ]

  for fname, action, reason in scenarios:
    path = os.path.join(G, fname)

    # check plain info
    expected_str1 = ".*local %s, incoming %s*" % (reason, action)
    expected_info = { 'Tree conflict' : expected_str1 }
    svntest.actions.run_and_verify_info([expected_info], path)

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

  # Check recursive info.  Just ensure that all victims are listed.
  exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                [], 'info',
                                                                G, '-R')
  for fname, action, reason in scenarios:
    found = False
    expected = ".*incoming %s.*" % (action)
    for item in output:
      if re.search(expected, item):
        found = True
        break
    if not found:
      raise svntest.verify.SVNUnexpectedStdout(
        "Tree conflict missing in svn info -R output:\n"
        "  Expected: '%s'\n"
        "  Found: '%s'" % (expected, output))

def info_on_added_file(sbox):
  """info on added file"""

  svntest.actions.make_repo_and_wc(sbox)
  wc_dir = sbox.wc_dir

  # create new file
  new_file = os.path.join(wc_dir, 'new_file')
  svntest.main.file_append(new_file, '')

  svntest.main.run_svn(None, 'add', new_file)

  uuid_regex = '[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}'

  # check that we have a Repository Root and Repository UUID
  expected = {'Path' : re.escape(new_file),
              'Name' : 'new_file',
              'URL' : '.*/new_file',
              'Repository Root' : '.*',
              'Revision' : '0',
              'Node Kind' : 'file',
              'Schedule' : 'add',
              'Repository UUID' : uuid_regex,
             }

  svntest.actions.run_and_verify_info([expected], new_file)

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

  uuid_regex = '[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}'

  # check that we have a Repository Root and Repository UUID
  expected = {'Path' : re.escape(new_dir),
              'URL' : '.*/new_dir',
              'Repository Root' : '.*',
              'Revision' : '0',
              'Node Kind' : 'directory',
              'Schedule' : 'add',
              'Repository UUID' : uuid_regex,
             }

  svntest.actions.run_and_verify_info([expected], new_dir)

  # check XML info
  exit_code, output, error = svntest.actions.run_and_verify_svn(None, None,
                                                                [], 'info',
                                                                new_dir,
                                                                '--xml')
  verify_xml_elements(output,
                      [('entry',    {'kind'     : 'dir',
                                     'path'     : new_dir,
                                     'revision' : '0'}),
                       ('url',      {}, '.*/new_dir'),
                       ('root',     {}, '.*'),
                       ('uuid',     {}, uuid_regex),
                       ('depth',    {}, 'infinity'),
                       ('schedule', {}, 'add')])

def info_wcroot_abspaths(sbox):
  """wc root paths in 'svn info' output"""

  def check_wcroot_paths(lines, wcroot_abspath):
    "check that paths found on input lines beginning 'Path: ' are as expected"
    path = None
    target = None
    for line in lines:
      if line.startswith('Path: '):
        target = line[6:].rstrip()
      if line.startswith('Working Copy Root Path: '):
        path = line[24:].rstrip()
      if target is not None and path is not None:
        break

    if target is None:
      target = "(UNKNOWN)"
      
    if path is None:
      print "No WC root path for '%s'" % (target)
      raise svntest.Failure
    
    if path != wcroot_abspath:
      print("For target '%s'..." % (target))
      print("   Reported WC root path: %s" % (path))
      print("   Expected WC root path: %s" % (wcroot_abspath))
      raise svntest.Failure

  sbox.build(read_only=True)
  exit_code, output, errput = svntest.main.run_svn(None, 'info', '-R', sbox.wc_dir)
  check_wcroot_paths(output, os.path.abspath(sbox.wc_dir))


########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              info_with_tree_conflicts,
              info_on_added_file,
              info_on_mkdir,
              info_wcroot_abspaths,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.

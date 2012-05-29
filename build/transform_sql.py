#!/usr/bin/env python
#
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
#
#
# transform_sql.py -- create a header file with the appropriate SQL variables
# from an SQL file
#


import os
import re
import sys


DEFINE_END = '  ""\n\n'


def usage_and_exit(msg):
  if msg:
    sys.stderr.write('%s\n\n' % msg)
  sys.stderr.write(
    'USAGE: %s SQLITE_FILE [OUTPUT_FILE]\n'
    '  stdout will be used if OUTPUT_FILE is not provided.\n'
    % os.path.basename(sys.argv[0]))
  sys.stderr.flush()
  sys.exit(1)


class Processor(object):
  re_comments = re.compile(r'/\*.*?\*/', re.MULTILINE|re.DOTALL)

  # a few SQL comments that act as directives for this transform system
  re_format = re.compile('-- *format: *([0-9]+)')
  re_statement = re.compile('-- *STMT_([A-Z_0-9]+)( +\(([^\)]*)\))?')
  re_include = re.compile('-- *include: *([-a-z]+)')
  re_define = re.compile('-- *define: *([A-Z_0-9]+)')

  def _sub_format(self, match):
    vsn = match.group(1)

    self.close_define()
    self.output.write('#define %s_%s \\\n' % (self.var_name, match.group(1)))
    self.var_printed = True

  def _sub_statement(self, match):
    name = match.group(1)

    self.close_define()
    self.output.write('#define STMT_%s %d\n' % (match.group(1),
                                                self.stmt_count))

    if match.group(3) == None:
      info = 'NULL'
    else:
      info = '"' + match.group(3) + '"'
    self.output.write('#define STMT_%d_INFO {"STMT_%s", %s}\n' %
                      (self.stmt_count, match.group(1), info))
    self.output.write('#define STMT_%d \\\n' % (self.stmt_count,))
    self.var_printed = True

    self.stmt_count += 1

  def _sub_include(self, match):
    filepath = os.path.join(self.dirpath, match.group(1) + '.sql')

    self.close_define()
    self.process_file(open(filepath).read())

  def _sub_define(self, match):
    define = match.group(1)

    self.output.write('  APR_STRINGIFY(%s) \\\n' % define)

  def __init__(self, dirpath, output, var_name):
    self.dirpath = dirpath
    self.output = output
    self.var_name = var_name

    self.stmt_count = 0
    self.var_printed = False

    self._directives = {
        self.re_format      : self._sub_format,
        self.re_statement   : self._sub_statement,
        self.re_include     : self._sub_include,
        self.re_define      : self._sub_define,
      }

  def process_file(self, input):
    input = self.re_comments.sub('', input)

    for line in input.split('\n'):
      line = line.replace('"', '\\"')

      # IS_STRICT_DESCENDANT_OF()

      # A common operation in the working copy is determining descendants of
      # a node. To allow Sqlite to use its indexes to provide the answer we
      # must provide simple less than and greater than operations.
      #
      # For relative paths that consist of one or more components like 'subdir'
      # we can accomplish this by comparing local_relpath with 'subdir/' and
      # 'subdir0' ('/'+1 = '0')
      #
      # For the working copy root this case is less simple and not strictly
      # valid utf-8/16 (but luckily Sqlite doesn't validate utf-8 nor utf-16).
      # The binary blob x'FFFF' is higher than any valid utf-8 and utf-16
      # sequence.
      #
      # So for the root we can compare with > '' and < x'FFFF'. (This skips the
      # root itself and selects all descendants)
      #
      ### RH: I implemented this first with a user defined Sqlite function. But
      ### when I wrote the documentation for it, I found out I could just
      ### define it this way, without losing the option of just dropping the
      ### query in a plain sqlite3.

      # '/'+1 == '0'
      line = re.sub(
            r'IS_STRICT_DESCENDANT_OF[(]([A-Za-z_.]+), ([?][0-9]+)[)]',
            r"(((\1) > (CASE (\2) WHEN '' THEN '' ELSE (\2) || '/' END))" +
            r" AND ((\1) < CASE (\2) WHEN '' THEN X'FFFF' ELSE (\2) || '0' END))",
            line)

      if line.strip():
        handled = False

        for regex, handler in self._directives.iteritems():
          match = regex.match(line)
          if match:
            handler(match)
            handled = True
            break

        # we've handed the line, so skip it
        if handled:
          continue

        if not self.var_printed:
          self.output.write('#define %s \\\n' % self.var_name)
          self.var_printed = True

        # got something besides whitespace. write it out. include some whitespace
        # to separate the SQL commands. and a backslash to continue the string
        # onto the next line.
        self.output.write('  "%s " \\\n' % line.rstrip())

    # previous line had a continuation. end the madness.
    self.close_define()

  def close_define(self):
    if self.var_printed:
      self.output.write(DEFINE_END)
      self.var_printed = False


def main(input_filepath, output):
  filename = os.path.basename(input_filepath)
  input = open(input_filepath, 'r').read()

  var_name = re.sub('[-.]', '_', filename).upper()

  output.write(
    '/* This file is automatically generated from %s.\n'
    ' * Do not edit this file -- edit the source and rerun gen-make.py */\n'
    '\n'
    % (filename,))

  proc = Processor(os.path.dirname(input_filepath), output, var_name)
  proc.process_file(input)

  ### the STMT_%d naming precludes *multiple* transform_sql headers from
  ### being used within the same .c file. for now, that's more than fine.
  ### in the future, we can always add a var_name discriminator or use
  ### the statement name itself (which should hopefully be unique across
  ### all names in use; or can easily be made so)
  if proc.stmt_count > 0:
    output.write(
      '#define %s_DECLARE_STATEMENTS(varname) \\\n' % (var_name,)
      + '  static const char * const varname[] = { \\\n'
      + ', \\\n'.join('    STMT_%d' % (i,) for i in range(proc.stmt_count))
      + ', \\\n    NULL \\\n  }\n')

    output.write('\n')

    output.write(
      '#define %s_DECLARE_STATEMENT_INFO(varname) \\\n' % (var_name,)
      + '  static const char * const varname[][2] = { \\\n'
      + ', \\\n'.join('    STMT_%d_INFO' % (i) for i in range(proc.stmt_count))
      + ', \\\n    {NULL, NULL} \\\n  }\n')

if __name__ == '__main__':
  if len(sys.argv) < 2 or len(sys.argv) > 3:
    usage_and_exit('Incorrect number of arguments')

  # Note: we could use stdin, but then we'd have no var_name
  input_filepath = sys.argv[1]

  if len(sys.argv) > 2:
    output_file = open(sys.argv[2], 'w')
  else:
    output_file = sys.stdout

  main(input_filepath, output_file)

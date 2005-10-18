#!/usr/bin/env python
#
#  testcase.py:  Control of test case execution.
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2004 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import os, sys, string
import traceback # for print_exc()

import svntest

__all__ = ['TestCase', 'XFail', 'Skip']


class SVNTestStatusCodeError(Exception):
  'Test driver returned a status code.'
  pass


class _Predicate:
  """A general-purpose predicate that encapsulates a test case (function),
  a condition for its execution and a set of display properties for test
  lists and test log output."""

  def __init__(self, func):
    if isinstance(func, _Predicate):
      # Whee, this is better than blessing objects in Perl!
      # For the unenlightened: What we're doing here is adopting the
      # identity *and class* of 'func'
      self.__dict__ = func.__dict__
      self.__class__ = func.__class__
    else:
      self.func = func
      self.cond = 0
      self.text = ['PASS: ', 'FAIL: ', 'SKIP: ', '']
    assert type(self.func) is type(lambda x: 0)

  def list_mode(self):
    return self.text[3]

  def skip_text(self):
    return self.text[2]

  def run_text(self, result=0):
    return self.text[result]

  def convert_result(self, result):
    return result


class TestCase:
  """Encapsulate a single test case (predicate), including logic for
  runing the test and test list output."""

  def __init__(self, func, index):
    self.pred = _Predicate(func)
    self.index = index

  def _check_name(self):
    name = self.pred.func.__doc__
    if len(name) > 50:
      print 'WARNING: Test docstring exceeds 50 characters'
    if name[-1] == '.':
      print 'WARNING: Test docstring ends in a period (.)'
    if not string.lower(name[0]) == name[0]:
      print 'WARNING: Test docstring is capitalized'
    
  def func_code(self):
    return self.pred.func.func_code

  def list(self):
    print " %2d     %-5s  %s" % (self.index,
                                 self.pred.list_mode(),
                                 self.pred.func.__doc__)
    self._check_name()
      
  def _print_name(self):
    print os.path.basename(sys.argv[0]), str(self.index) + ":", \
          self.pred.func.__doc__
    self._check_name()

  def run(self, args):
    """Run self.pred on ARGS, return the result.  The return value is
        - 0 if the test was successful
        - 1 if it errored in a way that indicates test failure
        - 2 if the test skipped
        """
    result = 0
    if self.pred.cond:
      print self.pred.skip_text(),
    else:
      try:
        rc = apply(self.pred.func, args)
        if rc is not None:
          raise SVNTestStatusCodeError
      except SVNTestStatusCodeError, ex:
        print "STYLE ERROR in",
        self._print_name()
        print ex.__doc__
        sys.exit(255)
      except svntest.Skip, ex:
        result = 2
      except svntest.Failure, ex:
        result = 1
        # We captured Failure and its subclasses. We don't want to print
        # anything for plain old Failure since that just indicates test
        # failure, rather than relevant information. However, if there
        # *is* information in the exception's arguments, then print it.
        if ex.__class__ != svntest.Failure or ex.args:
          ex_args = str(ex)
          if ex_args:
            print 'EXCEPTION: %s: %s' % (ex.__class__.__name__, ex_args)
          else:
            print 'EXCEPTION:', ex.__class__.__name__
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
      print self.pred.run_text(result),
      result = self.pred.convert_result(result)
    self._print_name()
    sys.stdout.flush()
    return result


class XFail(_Predicate):
  "A test that is expected to fail."

  def __init__(self, func):
    _Predicate.__init__(self, func)
    self.text[0] = 'XPASS:'
    self.text[1] = 'XFAIL:'
    if self.text[3] == '':
      self.text[3] = 'XFAIL'
  def convert_result(self, result):
    # Conditions are reversed here: a failure expected, therefore it
    # isn't an error; a pass is an error.
    return not result

class Skip(_Predicate):
  "A test that will be skipped when a condition is true."

  def __init__(self, func, cond):
    _Predicate.__init__(self, func)
    self.cond = cond
    if self.cond:
      self.text[3] = 'SKIP'


### End of file.

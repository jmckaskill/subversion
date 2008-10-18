#!/usr/bin/env python

import setup_path
import unittest
import locale
import os
import shutil
import tempfile
import StringIO
from csvn.core import *
from urllib import pathname2url
from csvn.wc import WC
from csvn.repos import LocalRepository

locale.setlocale(locale.LC_ALL, "C")

repos_location = os.path.join(tempfile.gettempdir(), "svn_test_repos")
wc_location = os.path.join(tempfile.gettempdir(), "svn_test_wc")
repo_url = pathname2url(repos_location)
if repo_url.startswith("///"):
  # Don't add extra slashes if they're already present.
  # (This is important for Windows compatibility).
  repo_url = "file:" + repo_url
else:
  # If the URL simply starts with '/', we need to add two
  # extra slashes to make it a valid 'file://' URL
  repo_url = "file://" + repo_url

if os.sep != "/":
    repos_location = repos_location.replace(os.sep, "/")
    wc_location = wc_location.replace(os.sep, "/")

class WCTestCase(unittest.TestCase):
    """Test case for Subversion WC layer."""
    
    def setUp(self):
        dumpfile = open(os.path.join(os.path.split(__file__)[0],
                        'test.dumpfile'))
                       
        # Just in case a preivous test instance was not properly cleaned up
        self.tearDown()
        self.repos = LocalRepository(repos_location, create=True)
        self.repos.load(dumpfile)
        
        self.wc = WC(wc_location)
        self.wc.checkout(repo_url)
        
    def tearDown(self):
        pool = Pool()
        if os.path.exists(wc_location):
            svn_io_remove_dir(wc_location, pool)
        if os.path.exists(repos_location):
            svn_repos_delete(repos_location, pool)
        self.wc = None
        
    def _info_reciever(self, path, info):
        self.last_info = info
        
    def test_info(self):
        self.wc.info(path="trunk/README.txt",info_func=self._info_reciever)
        self.assertEqual(9, self.last_info.rev)
        self.assertEqual(svn_node_file, self.last_info.kind)
        self.assertEqual(repo_url, self.last_info.repos_root_URL)
        self.assertEqual("890f2569-e600-4cfc-842a-f574dec58d87",
            self.last_info.repos_UUID)
        self.assertEqual(9, self.last_info.last_changed_rev)
        self.assertEqual("bruce", self.last_info.last_changed_author)
        self.assertEqual(-1, self.last_info.copyfrom_rev)
        
    def test_copy(self):
        self.wc.copy("trunk/README.txt", "trunk/DONTREADME.txt")
        self.wc.info(path="trunk/DONTREADME.txt",
            info_func=self._info_reciever)
        self.assertEqual(svn_wc_schedule_add, self.last_info.schedule)
        self.wc.info(path="trunk/README.txt")
        self.assertEqual(svn_wc_schedule_normal, self.last_info.schedule)
        
    def test_move(self):
        self.wc.move("trunk/README.txt", "trunk/DONTREADMEEITHER.txt")
        self.wc.info(path="trunk/DONTREADMEEITHER.txt",
            info_func=self._info_reciever)
        self.assertEqual(svn_wc_schedule_add, self.last_info.schedule)
        self.wc.info(path="trunk/README.txt")
        self.assertEqual(svn_wc_schedule_delete, self.last_info.schedule)
        
    def test_delete(self):
        self.wc.delete(["trunk/README.txt"])
        self.wc.info(path="trunk/README.txt",
            info_func=self._info_reciever)
        self.assertEqual(svn_wc_schedule_delete, self.last_info.schedule)
        
    def test_mkdir(self):
        self.wc.mkdir(["trunk/plank"])
        self.wc.info(path="trunk/plank",
            info_func=self._info_reciever)
        self.assertEqual(svn_wc_schedule_add, self.last_info.schedule)
        
    def test_add(self):
        f = open("%s/trunk/ADDED.txt" % wc_location, "w")
        f.write("Something")
        f.close()
        
        self.wc.add("trunk/ADDED.txt")
        self.wc.info(path="trunk/ADDED.txt",
            info_func=self._info_reciever)
        self.assertEqual(svn_wc_schedule_add, self.last_info.schedule)
        
    def test_revert(self):
        self.wc.revert([""],True)
        self.wc.info(path="trunk/README.txt",
            info_func=self._info_reciever)
        self.assertEqual(svn_wc_schedule_normal, self.last_info.schedule)
        
    def test_diff(self):
        path = "%s/trunk/README.txt" % wc_location

        diffstring="""Index: """+path+"""
===================================================================
--- """+path+"""\t(revision 9)
+++ """+path+"""\t(working copy)
@@ -1,7 +0,0 @@
-This repository is for test purposes only. Any resemblance to any other
-repository, real or imagined, is purely coincidental.
-
-Contributors:
-Clark
-Bruce
-Henry
"""
        f = open(path, "w")
        f.truncate(0)
        f.close()
        difffile = StringIO.StringIO()
        self.wc.diff("trunk", outfile=difffile)
        difffile.seek(0)
        diffresult = difffile.read().replace("\r","")
        self.assertEqual(diffstring, diffresult)
        
        path = "%s/branches/0.x/README.txt" % wc_location
        diffstring="""Index: """+path+"""
===================================================================
--- """+path+"""\t(revision 0)
+++ """+path+"""\t(revision 5)
@@ -0,0 +1,9 @@
+This repository is for test purposes only. Any resemblance to any other
+repository, real or imagined, is purely coincidental.
+
+This branch preserves and refines the code of the excellent pre-1.0 days.
+
+Contributors:
+Clark
+Bruce
+Henry
"""
        difffile.seek(0)
        self.wc.diff(revnum1=4, revnum2=5, outfile=difffile)
        difffile.seek(0)
        diffresult = difffile.read().replace("\r","")
        self.assertEqual(diffstring, diffresult)
        
        
    def test_export(self):
        export_location = os.path.join(tempfile.gettempdir(), "svn_export")
        self.wc.export("", export_location)
        if not os.path.exists(export_location):
            self.fail("Export directory does not exist")
        else:
            shutil.rmtree(export_location)
            
    def test_propget(self):
        props = self.wc.propget("Awesome")
        path = "%s/trunk/README.txt" % wc_location
        if not path in props.keys():
            self.fail("File missing in propget")
            
    def test_propset(self):
        self.wc.propset("testprop", "testval", "branches/0.x/README.txt")
        props = self.wc.propget("testprop", "branches/0.x/README.txt")
        if not "%s/branches/0.x/README.txt" % wc_location in \
                props.keys():
                    
            self.fail("Property not set")
            
    def test_update(self):
        path = "trunk/README.txt"
        results = self.wc.update([path], revnum=7)
        self.assertEqual(results[0], 7)
        props = self.wc.propget("Awesome")
        if "%s/%s" % (wc_location, path) in \
                props.keys():
            self.fail("File not updated to old revision")
        results = self.wc.update([path])
        self.assertEqual(results[0], 9)
        self.test_propget()

    def test_switch(self):
        self.wc.switch("trunk", "%s/tags" % repo_url)
        if os.path.exists("%s/trunk/README.txt" % wc_location):
            self.fail("Switch did not happen")
            
    def test_lock(self):
        self.wc.lock(["%s/trunk/README.txt" % wc_location],
                        "Test lock")
        self.wc.info(path="trunk/README.txt",
            info_func=self._info_reciever)
        if not self.last_info.lock:
            self.fail("Lock not aquired")
            
    def test_unlock(self):
        path = "%s/trunk/README.txt" % wc_location
        self.wc.lock([path], "Test lock")
        self.wc.info(path=path,
            info_func=self._info_reciever)
        if not self.last_info.lock:
            self.fail("Lock not aquired")
        self.wc.unlock([path])
        
        self.wc.info(path="trunk/README.txt",
            info_func=self._info_reciever)
        if self.last_info.lock:
            self.fail("Lock not released")
        

def suite():
    return unittest.makeSuite(WCTestCase, 'test')

if __name__ == '__main__':
    runner = unittest.TextTestRunner(verbosity=2)
    runner.run(suite())

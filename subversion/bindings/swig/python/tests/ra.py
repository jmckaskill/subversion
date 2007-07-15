import unittest, os, setup_path

from svn import core, repos, fs, delta, client, ra
from StringIO import StringIO

from trac.versioncontrol.tests.svn_fs import SubversionRepositoryTestSetup, \
  REPOS_PATH, REPOS_URL

class SubversionRepositoryAccessTestCase(unittest.TestCase):
  """Test cases for the Subversion repository layer"""

  def setUp(self):
    """Load a Subversion repository"""

    ra.initialize()

    # Open repository directly for cross-checking
    self.repos = repos.open(REPOS_PATH)
    self.fs = repos.fs(self.repos)

    callbacks = ra.callbacks2_t()

    self.ra_ctx = ra.open2(REPOS_URL, callbacks, None, None)

  def test_get_file(self):
    # Test getting the properties of a file
    fs_revnum = fs.youngest_rev(self.fs)
    rev, properties = ra.get_file(self.ra_ctx, "trunk/README2.txt",
                                  core.svn_invalid_revnum, None)
    self.assertEqual(rev, fs_revnum)
    self.assertEqual(properties["svn:mime-type"], "text/plain")

    # Test getting the contents of a file
    filestream = StringIO()
    rev, properties = ra.get_file(self.ra_ctx, "trunk/README2.txt",
                                  fs_revnum, filestream)
    self.assertEqual("A test.\n", filestream.getvalue())

  def test_get_repos_root(self):
    root = ra.get_repos_root(self.ra_ctx)
    self.assertEqual(root,REPOS_URL)

  def test_get_uuid(self):
    ra_uuid = ra.get_uuid(self.ra_ctx)
    fs_uuid = fs.get_uuid(self.fs)
    self.assertEqual(ra_uuid,fs_uuid)

  def test_get_latest_revnum(self):
    ra_revnum = ra.get_latest_revnum(self.ra_ctx)
    fs_revnum = fs.youngest_rev(self.fs)
    self.assertEqual(ra_revnum,fs_revnum)

  def test_get_dir2(self):
    (dirents,_,props) = ra.get_dir2(self.ra_ctx, '', 1, core.SVN_DIRENT_KIND)
    self.assert_(dirents.has_key('trunk'))
    self.assert_(dirents.has_key('branches'))
    self.assert_(dirents.has_key('tags'))
    self.assertEqual(dirents['trunk'].kind, core.svn_node_dir)
    self.assertEqual(dirents['branches'].kind, core.svn_node_dir)
    self.assertEqual(dirents['tags'].kind, core.svn_node_dir)
    self.assert_(props.has_key(core.SVN_PROP_ENTRY_UUID))
    self.assert_(props.has_key(core.SVN_PROP_ENTRY_LAST_AUTHOR))

    (dirents,_,_) = ra.get_dir2(self.ra_ctx, 'trunk', 1, core.SVN_DIRENT_KIND)

    self.assertEqual(dirents, {})

    (dirents,_,_) = ra.get_dir2(self.ra_ctx, 'trunk', 10, core.SVN_DIRENT_KIND)

    self.assert_(dirents.has_key('README2.txt'))
    self.assertEqual(dirents['README2.txt'].kind,core.svn_node_file)

  def test_commit3(self):
    commit_info = []
    def my_callback(info, pool):
      commit_info.append(info)

    revprops = {"svn:log": "foobar", "testprop": ""}
    editor, edit_baton = ra.get_commit_editor3(self.ra_ctx, revprops, my_callback, None, False)
    root = editor.open_root(edit_baton, 4)
    self.assertNotEqual(root, None)
    child = editor.add_directory("bla3", root, None, 0)
    self.assertNotEqual(child, None)
    editor.close_edit(edit_baton)

    info = commit_info[0]
    self.assertEqual(info.revision, fs.youngest_rev(self.fs))
    revprops['svn:author'] = info.author
    revprops['svn:date'] = info.date
    self.assertEqual(ra.rev_proplist(self.ra_ctx, info.revision), revprops)

  def test_commit2(self):
    def my_callback(info, pool):
        self.assertEqual(info.revision, fs.youngest_rev(self.fs))

    editor, edit_baton = ra.get_commit_editor2(self.ra_ctx, "foobar", my_callback, None, False)
    root = editor.open_root(edit_baton, 4)
    self.assertNotEqual(root, None)
    child = editor.add_directory("bla", root, None, 0)
    self.assertNotEqual(child, None)
    editor.close_edit(edit_baton)

  def test_commit(self):
    def my_callback(revision, date, author):
        self.assertEqual(revision, fs.youngest_rev(self.fs))

    editor, edit_baton = ra.get_commit_editor(self.ra_ctx, "foobar", my_callback, None, False)
    root = editor.open_root(edit_baton, 4)
    child = editor.add_directory("blah", root, None, 0)
    editor.close_edit(edit_baton)

  def test_do_diff2(self):

    class ChangeReceiver(delta.Editor):
        def __init__(self):
            self.textdeltas = []
        
        def apply_textdelta(self, file_baton, base_checksum):
            def textdelta_handler(textdelta):
                if textdelta is not None:
                    self.textdeltas.append(textdelta)
            return textdelta_handler

    editor = ChangeReceiver()

    e_ptr, e_baton = delta.make_editor(editor)

    fs_revnum = fs.youngest_rev(self.fs)

    reporter, reporter_baton = ra.do_diff2(self.ra_ctx, fs_revnum, REPOS_URL + "/trunk/README.txt", 0, 0, 1, REPOS_URL + "/trunk/README.txt", e_ptr, e_baton)

    reporter.set_path(reporter_baton, "", fs_revnum, True, None)

    reporter.finish_report(reporter_baton)

    self.assertEqual("A test.\n", editor.textdeltas[0].new_data)
    self.assertEqual(1, len(editor.textdeltas))

  def test_get_locations(self):
    locations = ra.get_locations(self.ra_ctx, "/trunk/README.txt", 2, range(1,5))
    self.assertEqual(locations, {
        2: '/trunk/README.txt', 
        3: '/trunk/README.txt', 
        4: '/trunk/README.txt'})

  def test_get_file_revs(self):
    def rev_handler(path, rev, rev_props, prop_diffs, pool):
        self.assert_(rev == 2 or rev == 3)
        self.assertEqual(path, "/trunk/README.txt")
        if rev == 2:
            self.assertEqual(rev_props, {
              'svn:log': 'Added README.',
              'svn:author': 'john',
              'svn:date': '2005-04-01T13:12:18.216267Z'
            })
            self.assertEqual(prop_diffs, {})
        elif rev == 3:
            self.assertEqual(rev_props, {
              'svn:log': 'Fixed README.\n',
              'svn:author': 'kate',
              'svn:date': '2005-04-01T13:24:58.234643Z'
            })
            self.assertEqual(prop_diffs, {'svn:mime-type': 'text/plain', 'svn:eol-style': 'native'})

    ra.get_file_revs(self.ra_ctx, "trunk/README.txt", 0, 10, rev_handler)

  def test_update(self):
    class TestEditor(delta.Editor):
        pass

    editor = TestEditor()

    e_ptr, e_baton = delta.make_editor(editor)
    
    reporter, reporter_baton = ra.do_update(self.ra_ctx, 10, "", True, e_ptr, e_baton)

    reporter.set_path(reporter_baton, "", 0, True, None)

    reporter.finish_report(reporter_baton)

def suite():
    return unittest.makeSuite(SubversionRepositoryAccessTestCase, 'test',
                              suiteClass=SubversionRepositoryTestSetup)

if __name__ == '__main__':
    runner = unittest.TextTestRunner()
    runner.run(suite())

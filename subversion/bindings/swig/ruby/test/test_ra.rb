require "util"

require "svn/ra"

class SvnRaTest < Test::Unit::TestCase
  include SvnTestUtil
  
  def setup
    setup_basic
  end

  def teardown
    teardown_basic
  end

  def test_version
    assert_equal(Svn::Core.subr_version, Svn::Ra.version)
  end

  def test_session
    log = "sample log"
    log2 = "sample log2"
    file = "sample.txt"
    src = "sample source"
    path = File.join(@wc_path, file)
    path_in_repos = "/#{file}"
    ctx = make_context(log)
    config = {}
    path_props = {"my-prop" => "value"}
    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)

    assert_equal(youngest_rev, session.latest_revnum)
    assert_equal(@repos_uri, session.repos_root)

    File.open(path, "w") {|f| f.print(src)}
    ctx.add(path)
    info = ctx.ci(@wc_path)
    rev1 = info.revision

    assert_equal(info.revision, session.dated_revision(info.date))
    content, props = session.file(path_in_repos, info.revision)
    assert_equal(src, content)
    assert_equal([
                   Svn::Core::PROP_ENTRY_COMMITTED_DATE,
                   Svn::Core::PROP_ENTRY_UUID,
                   Svn::Core::PROP_ENTRY_LAST_AUTHOR,
                   Svn::Core::PROP_ENTRY_COMMITTED_REV,
                 ].sort,
                 props.keys.sort)
    
    entries, props = session.dir("/", info.revision)
    assert_equal([file], entries.keys)
    assert(entries[file].file?)
    assert_equal([
                   Svn::Core::PROP_ENTRY_COMMITTED_DATE,
                   Svn::Core::PROP_ENTRY_UUID,
                   Svn::Core::PROP_ENTRY_LAST_AUTHOR,
                   Svn::Core::PROP_ENTRY_COMMITTED_REV,
                 ].sort,
                 props.keys.sort)

    entries, props = session.dir("/", info.revision, Svn::Core::DIRENT_KIND)
    assert_equal(Svn::Core::NODE_FILE, entries[file].kind)
    entries, props = session.dir("/", info.revision, 0)
    assert_equal(Svn::Core::NODE_NONE, entries[file].kind)

    ctx = make_context(log2)
    File.open(path, "w") {|f| f.print(src * 2)}
    path_props.each do |key, value|
      ctx.prop_set(key, value, path)
    end
    info = ctx.ci(@wc_path)
    rev2 = info.revision

    logs = []
    receiver = Proc.new do |changed_paths, revision, author, date, message|
      logs << [revision, message]
    end
    session.log([file], rev1, rev2, rev2 - rev1 + 1, &receiver)
    assert_equal([
                   [rev1, log],
                   [rev2, log2],
                 ].sort_by {|rev, log| rev},
                 logs.sort_by {|rev, log| rev})
    
    assert_equal(Svn::Core::NODE_FILE, session.check_path(file))
    assert_equal(Svn::Core::NODE_FILE, session.stat(file).kind)

    assert_equal({
                   rev1 => "/#{file}",
                   rev2 => "/#{file}",
                 },
                 session.locations(file, [rev1, rev2]))

    infos = []
    session.file_revs(file, rev1, rev2) do |_path, rev, rev_props, prop_diffs|
      hashed_prop_diffs = {}
      prop_diffs.each do |prop|
        hashed_prop_diffs[prop.name] = prop.value
      end
      infos << [rev, _path, hashed_prop_diffs]
    end
    assert_equal([
                   [rev1, path_in_repos, {}],
                   [rev2, path_in_repos, path_props],
                 ],
                 infos)

    infos = []
    session.file_revs2(file, rev1, rev2) do |_path, rev, rev_props, prop_diffs|
      infos << [rev, _path, prop_diffs]
    end
    assert_equal([
                   [rev1, path_in_repos, {}],
                   [rev2, path_in_repos, path_props],
                 ],
                 infos)

    assert_equal({}, session.get_locks("/"))
    locks = []
    session.lock({path_in_repos => rev2}) do |_path, do_lock, lock, ra_err|
      locks << [_path, do_lock, lock, ra_err]
    end
    assert_equal([path_in_repos],
                 locks.collect{|_path, *rest| _path}.sort)
    lock = locks.assoc(path_in_repos)[2]
    assert_equal([path_in_repos],
                 session.get_locks("/").collect{|_path, *rest| _path})
    assert_equal(lock.token, session.get_lock(file).token)
    assert_equal([lock.token],
                 session.get_locks(file).values.collect{|l| l.token})
    session.unlock({file => lock.token})
    assert_equal({}, session.get_locks(file))
  end

  def test_prop
    log = "sample log"
    file = "sample.txt"
    path = File.join(@wc_path, file)
    ctx = make_context(log)
    config = {}
    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)

    FileUtils.touch(path)
    ctx.add(path)
    info = ctx.commit(@wc_path)

    assert_equal(@author, session.prop(Svn::Core::PROP_REVISION_AUTHOR))
    assert_equal(log, session.prop(Svn::Core::PROP_REVISION_LOG))
    assert_equal([
                   Svn::Core::PROP_REVISION_AUTHOR,
                   Svn::Core::PROP_REVISION_DATE,
                   Svn::Core::PROP_REVISION_LOG,
                 ].sort,
                 session.proplist.keys.sort)
    session.set_prop(Svn::Core::PROP_REVISION_LOG, nil)
    assert_nil(session.prop(Svn::Core::PROP_REVISION_LOG))
    assert_equal([
                   Svn::Core::PROP_REVISION_AUTHOR,
                   Svn::Core::PROP_REVISION_DATE,
                 ].sort,
                 session.proplist.keys.sort)
  end

  def test_callback
    log = "sample log"
    file = "sample.txt"
    src1 = "sample source1"
    src2 = "sample source2"
    path = File.join(@wc_path, file)
    path_in_repos = "/#{file}"
    ctx = make_context(log)
    config = {}
    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)

    File.open(path, "w") {|f| f.print(src1)}
    ctx.add(path)
    rev1 = ctx.ci(@wc_path).revision

    File.open(path, "w") {|f| f.print(src2)}
    rev2 = ctx.ci(@wc_path).revision

    ctx.up(@wc_path)

    editor, editor_baton = session.commit_editor(log) {}
    reporter = session.update(rev2, "/", editor, editor_baton)
    reporter.abort_report

    editor, editor_baton = session.commit_editor(log) {}
    reporter = session.update2(rev2, "/", editor)
    reporter.abort_report
  end

  def test_diff
    log = "sample log"
    file = "sample.txt"
    src1 = "a\nb\nc\nd\ne\n"
    src2 = "a\nb\nC\nd\ne\n"
    path = File.join(@wc_path, file)
    path_in_repos = "/#{file}"
    ctx = make_context(log)
    config = {}
    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)

    File.open(path, "w") {|f| f.print(src1)}
    ctx.add(path)
    rev1 = ctx.ci(@wc_path).revision

    File.open(path, "w") {|f| f.print(src2)}
    rev2 = ctx.ci(@wc_path).revision

    ctx.up(@wc_path)

    editor = Svn::Delta::BaseEditor.new # dummy
    reporter = session.diff(rev2, "", @repos_uri, editor)
    reporter.set_path("", rev1, false, nil)
    reporter.finish_report
  end

  def test_commit_editor
    log = "sample log"
    dir_uri = "/test"
    config = {}
    ctx = make_context(log)
    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)
    actual_info = nil
    actual_date = nil

    expected_info = [1, @author]
    start_time = Time.now
    gc_disable do
      editor, baton = session.commit_editor(log) do |rev, date, author|
        actual_info = [rev, author]
        actual_date = date
      end
      editor.baton = baton

      root = editor.open_root(-1)
      editor.add_directory(dir_uri, root, nil, -1)
      gc_enable do
        GC.start
        editor.close_edit
      end
      assert_equal(expected_info, actual_info)
      assert_operator(start_time..(Time.now), :include?, actual_date)
    end
  end

  def test_commit_editor2
    log = "sample log"
    dir_uri = "/test"
    config = {}
    ctx = make_context(log)
    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)
    actual_info = nil
    actual_date = nil

    expected_info = [1, @author]
    start_time = Time.now
    gc_disable do
      editor = session.commit_editor2(log) do |info|
        actual_info = [info.revision, info.author]
        actual_date = info.date
      end

      root = editor.open_root(-1)
      editor.add_directory(dir_uri, root, nil, -1)
      gc_enable do
        GC.start
        editor.close_edit
      end
      assert_equal(expected_info, actual_info)
      assert_operator(start_time..(Time.now), :include?, actual_date)
    end
  end

  def test_reparent
    log = "sample log"
    dir = "dir"
    deep_dir = "deep"
    dir_path = File.join(@wc_path, dir)
    deep_dir_path = File.join(dir_path, deep_dir)
    config = {}
    ctx = make_context(log)

    ctx.mkdir(dir_path)
    ctx.ci(@wc_path)

    ctx.mkdir(deep_dir_path)
    ctx.ci(@wc_path)

    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)

    entries, props = session.dir(dir, nil)
    assert_equal([deep_dir], entries.keys)
    assert_raise(Svn::Error::FS_NOT_FOUND) do
      session.dir(deep_dir)
    end

    session.reparent("#{@repos_uri}/#{dir}")
    assert_raise(Svn::Error::FS_NOT_FOUND) do
      session.dir(dir)
    end
    entries, props = session.dir(deep_dir)
    assert_equal([], entries.keys)

    assert_raise(Svn::Error::RA_ILLEGAL_URL) do
      session.reparent("file:///tmp/xxx")
    end
  end

  def test_merge_info
    log = "sample log"
    file = "sample.txt"
    src = "sample\n"
    config = {}
    trunk = File.join(@wc_path, "trunk")
    branch = File.join(@wc_path, "branch")
    trunk_path = File.join(trunk, file)
    branch_path = File.join(branch, file)
    trunk_uri = "/trunk"
    branch_uri = "/branch"

    ctx = make_context(log)
    ctx.mkdir(trunk, branch)
    File.open(trunk_path, "w") {}
    File.open(branch_path, "w") {}
    ctx.add(trunk_path)
    ctx.add(branch_path)
    rev1 = ctx.commit(@wc_path).revision

    File.open(branch_path, "w") {|f| f.print(src)}
    rev2 = ctx.commit(@wc_path).revision

    callbacks = Svn::Ra::Callbacks.new(ctx.auth_baton)
    session = Svn::Ra::Session.open(@repos_uri, config, callbacks)

    assert_nil(session.merge_info(trunk_uri))
    ctx.merge(branch, rev1, branch, rev2, trunk)
    assert_nil(session.merge_info(trunk_uri))

    rev3 = ctx.commit(@wc_path).revision
    merge_info = session.merge_info(trunk_uri)
    assert_equal([trunk_uri], merge_info.keys)
    trunk_merge_info = merge_info[trunk_uri]
    assert_equal([branch_uri], trunk_merge_info.keys)
    assert_equal([[1, 2]],
                 trunk_merge_info[branch_uri].collect {|range| range.to_a})

    ctx.rm(branch_path)
    rev4 = ctx.commit(@wc_path).revision

    ctx.merge(branch, rev3, branch, rev4, trunk)
    assert(!File.exist?(trunk_path))
    rev5 = ctx.commit(@wc_path).revision

    merge_info = session.merge_info(trunk_uri, rev5)
    assert_equal([trunk_uri], merge_info.keys)
    trunk_merge_info = merge_info[trunk_uri]
    assert_equal([branch_uri], trunk_merge_info.keys)
    assert_equal([[1, 2], [3, 4]],
                 trunk_merge_info[branch_uri].collect {|range| range.to_a})
  end
end

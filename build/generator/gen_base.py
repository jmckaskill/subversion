#
# gen_base.py -- infrastructure for generating makefiles, dependencies, etc.
#

import os
import sys
import string
import glob
import re
import fileinput
import ConfigParser

import getversion


class GeneratorBase:

  #
  # Derived classes should define a class attribute named _extension_map.
  # This attribute should be a dictionary of the form:
  #     { (target-type, file-type): file-extension ...}
  #
  # where: target-type is 'exe', 'lib', ...
  #        file-type is 'target', 'object', ...
  #

  def __init__(self, fname, verfname, options=None):
    parser = ConfigParser.ConfigParser()
    parser.read(fname)

    if not hasattr(self, 'apr_path'):
      self.apr_path = 'apr'
    if not hasattr(self, 'apr_util_path'):
      self.apr_util_path = 'apr-util'
    if not hasattr(self, 'apr_iconv_path'):
      self.apr_iconv_path = 'apr-iconv'

    self.cfg = Config()
    self.cfg.swig_lang = string.split(parser.get('options', 'swig-languages'))
    self.cfg.apr_path = self.apr_path
    self.cfg.apr_util_path =  self.apr_util_path
    self.cfg.apr_iconv_path = self.apr_iconv_path

    # Version comes from a header file since it is used in the code.
    try:
      vsn_parser = getversion.Parser()
      vsn_parser.search('SVN_VER_LIBRARY', 'libver')
      self.cfg.version = vsn_parser.parse(verfname).libver
    except:
      raise GenError('Unable to extract version.')

    self.sections = { }
    self.graph = DependencyGraph()

    if not hasattr(self, 'skip_sections'):
      self.skip_sections = { }

    # PASS 1: collect the targets and some basic info
    for section_name in _filter_sections(parser.sections()):
      if self.skip_sections.has_key(section_name):
        continue

      options = {}
      for option in parser.options(section_name):
        options[option] = parser.get(section_name, option)

      type = options.get('type')

      target_class = _build_types.get(type)
      if not target_class:
        raise GenError('ERROR: unknown build type: ' + type)

      section = target_class.Section(options, target_class)

      self.sections[section_name] = section

      section.create_targets(self.graph, section_name, self.cfg,
                             self._extension_map)

    # compute intra-library dependencies
    for section in self.sections.values():
      dep_types = ((DT_LINK, section.options.get('libs')),
                   (DT_NONLIB, section.options.get('nonlibs')))

      for dt_type, deps_list in dep_types:
        if deps_list:
          for dep_section in self.find_sections(deps_list):            
            for target in section.get_targets():
              self.graph.bulk_add(dt_type, target.name,
                                  dep_section.get_dep_targets(target))

    # collect various files
    self.includes = _collect_paths(parser.get('options', 'includes'))
    self.apache_files = _collect_paths(parser.get('static-apache', 'paths'))

    # collect all the test scripts
    self.scripts = _collect_paths(parser.get('test-scripts', 'paths'))
    self.fs_scripts = _collect_paths(parser.get('fs-test-scripts', 'paths'))

    self.swig_dirs = string.split(parser.get('swig-dirs', 'paths'))

  def find_sections(self, section_list):
    """Return a list of section objects from a string of section names."""
    sections = [ ]
    for section_name in string.split(section_list):
      if not self.skip_sections.has_key(section_name):
        sections.append(self.sections[section_name])
    return sections

  def compute_hdr_deps(self):
    #
    # Find all the available headers and what they depend upon. the
    # include_deps is a dictionary mapping a short header name to a tuple
    # of the full path to the header and a dictionary of dependent header
    # names (short) mapping to None.
    #
    # Example:
    #   { 'short.h' : ('/path/to/short.h',
    #                  { 'other.h' : None, 'foo.h' : None }) }
    #
    # Note that this structure does not allow for similarly named headers
    # in per-project directories. SVN doesn't have this at this time, so
    # this structure works quite fine. (the alternative would be to use
    # the full pathname for the key, but that is actually a bit harder to
    # work with since we only see short names when scanning, and keeping
    # a second variable around for mapping the short to long names is more
    # than I cared to do right now)
    #
    include_deps = _create_include_deps(self.includes)
    for d in unique(self.graph.get_sources(DT_LIST, LT_TARGET_DIRS)):
      hdrs = glob.glob(os.path.join(d, '*.h'))
      if hdrs:
        more_deps = _create_include_deps(hdrs, include_deps)
        include_deps.update(more_deps)

    for objname, sources in self.graph.get_deps(DT_OBJECT):
      assert len(sources) == 1
      if isinstance(objname, SWIGObject):
        for ifile in self.graph.get_sources(DT_SWIG_C, sources[0]):
          if isinstance(ifile, SWIGSource):
            for short in _find_includes(ifile.filename, include_deps):
              self.graph.add(DT_SWIG_C, sources[0], include_deps[short][0])
        continue

      if isinstance(sources[0], ObjectFile) or \
        isinstance(sources[0], HeaderFile):
        if sources[0].source_generated == 1:
          continue

      hdrs = [ ]
      for short in _find_includes(sources[0].filename, include_deps):
        self.graph.add(DT_OBJECT, objname, include_deps[short][0])


class DependencyGraph:
  """Record dependencies between build items.

  See the DT_* values for the different dependency types. For each type,
  the target and source objects recorded will be different. They could
  be file names, Target objects, install types, etc.
  """

  def __init__(self):
    self.deps = { }     # type -> { target -> [ source ... ] }
    for dt in dep_types:
      self.deps[dt] = { }

  def add(self, type, target, source):
    if self.deps[type].has_key(target):
      self.deps[type][target].append(source)
    else:
      self.deps[type][target] = [ source ]
      
  def bulk_add(self, type, target, sources):
    if self.deps[type].has_key(target):
      self.deps[type][target].extend(sources)
    else:
      self.deps[type][target] = sources[:]  

  def get_sources(self, type, target, cls=None):
    sources = self.deps[type].get(target, [ ])
    if not cls:
      return sources
    filtered = [ ]
    for src in sources:
      if isinstance(src, cls):
        filtered.append(src)
    return filtered

  def get_all_sources(self, type):
    sources = [ ]
    for group in self.deps[type].values():
      sources.extend(group)
    sources.sort()  # ensures consistency between runs
    return sources

  def get_targets(self, type):
    targets = self.deps[type].keys()
    targets.sort()  # ensures consistency between runs
    return targets

  def get_deps(self, type):
    deps = self.deps[type].items()
    deps.sort()  # ensures consistency between runs

    ### this function is handy for looking at differences between running
    ### gen-make using different versions of Python. the dictionary
    ### algorithm changes periodically, so items are inserted into the
    ### dependency list in different orders. we do not want to keep this
    ### enabled, however, as it makes assumptions that the first dependency
    ### is a source file. in fact, other parts of the code assume that, too,
    ### so a dumb sort is not possible: we have to sort [1:]. but for
    ### testing purposes, to look at the output, this can be handy. just
    ### uncomment the map() call as needed.
    def test_sorter((objname, sources)):
      first = sources[0]
      del sources[0]
      sources.sort()
      sources.insert(0, first)
    #map(test_sorter, deps)

    return deps

# dependency types
dep_types = [
  'DT_INSTALL',  # install areas. e.g. 'lib', 'base-lib', 'fs-lib'
  'DT_OBJECT',   # an object filename, depending upon .c filenames
  'DT_SWIG_C',   # a swig-generated .c file, depending upon .i filename(s)
  'DT_LINK',     # a libtool-linked filename, depending upon object fnames
  'DT_INCLUDE',  # filename includes (depends) on sources (all basenames)
  'DT_NONLIB',   # filename depends on object fnames, but isn't linked to them
  'DT_LIST',     # arbitrary listS of values, see list_types below
  ]

list_types = [
  'LT_PROJECT',       # Visual C++ projects (TargetSpecial instances)
  'LT_TEST_DEPS',     # Test programs to build
  'LT_TEST_PROGS',    # Test programs to run (subset of LT_TEST_DEPS)
  'LT_FS_TEST_DEPS',  # File system test programs to build
  'LT_FS_TEST_PROGS', # File system test programs to run
  'LT_TARGET_DIRS',   # directories where files are built
  'LT_MANPAGES',      # manpages
  ]

# create some variables for these
for _dt in dep_types + list_types:
  # e.g. DT_INSTALL = 'DT_INSTALL'
  globals()[_dt] = _dt

class DependencyNode:
  def __init__(self, filename):
    self.filename = filename

  def __str__(self):
    return self.filename

  def __cmp__(self, ob):
    return cmp(self.filename, ob.filename)

  def __hash__(self):
    return hash(self.filename)

class ObjectFile(DependencyNode):
  def __init__(self, filename, compile_cmd = None):
    DependencyNode.__init__(self, filename)
    self.compile_cmd = compile_cmd
    self.source_generated = 0

class SWIGObject(ObjectFile):
  def __init__(self, filename, lang):
    ObjectFile.__init__(self, filename)
    self.lang = lang
    self.lang_abbrev = lang_abbrev[lang]
    ### hmm. this is Makefile-specific
    self.compile_cmd = '$(COMPILE_%s_WRAPPER)' % string.upper(self.lang_abbrev)
    self.source_generated = 1

class HeaderFile(DependencyNode):
  def __init__(self, filename, classname = None, compile_cmd = None):
    DependencyNode.__init__(self, filename)
    self.classname = classname
    self.compile_cmd = compile_cmd

class SourceFile(DependencyNode):
  def __init__(self, filename, reldir):
    DependencyNode.__init__(self, filename)
    self.reldir = reldir
class SWIGSource(SourceFile):
  def __init__(self, filename):
    SourceFile.__init__(self, filename, os.path.dirname(filename))
  pass

lang_abbrev = {
  'python' : 'py',
  'java' : 'java',
  'perl' : 'pl',
  'ruby' : 'rb',
  'tcl' : 'tcl',
  ### what others?
  }

lang_full_name = {
  'python' : 'Python',
  'java' : 'Java',
  'perl' : 'Perl',
  'ruby' : 'Ruby',
  'tcl' : 'TCL',
  ### what others?
  }

class Target(DependencyNode):
  "A build target is a node in our dependency graph."

  def __init__(self, name, options, cfg, extmap):
    self.name = name
    self.desc = options.get('description')
    self.path = options.get('path', '')
    self.add_deps = options.get('add-deps', '')
    self.msvc_name = options.get('msvc-name') # override project name

    # true if several targets share the same directory, as is the case
    # with SWIG bindings.
    self.shared_dir = None

  def add_dependencies(self, graph, cfg, extmap):
    # subclasses should override to provide behavior, as appropriate
    raise NotImplementedError

  class Section:
    """Represents an individual section of build.conf
    
    The Section class is sort of a factory class which is responsible for
    creating and keeping track of Target instances associated with a section
    of the configuration file. By default it only allows one Target per 
    section, but subclasses may create multiple Targets.
    """

    def __init__(self, options, target_class):
      self.options = options
      self.target_class = target_class

    def create_targets(self, graph, name, cfg, extmap):
      """Create target instances"""
      self.target = self.target_class(name, self.options, cfg, extmap)
      self.target.add_dependencies(graph, cfg, extmap)

    def get_targets(self):
      """Return list of target instances associated with this section"""
      return [self.target]

    def get_dep_targets(self, target):
      """Return list of targets from this section that "target" depends on"""
      return [self.target]

class TargetLinked(Target):
  "The target is linked (by libtool) against other libraries."

  def __init__(self, name, options, cfg, extmap):
    Target.__init__(self, name, options, cfg, extmap)
    self.install = options.get('install')
    self.compile_cmd = options.get('compile-cmd')
    self.sources = options.get('sources', '*.c')
    self.link_cmd = options.get('link-cmd', '$(LINK)')

    self.external_lib = options.get('external-lib')
    self.external_project = options.get('external-project')
    self.msvc_libs = string.split(options.get('msvc-libs', ''))

    if self.external_project:
      if self.external_project[:9] == 'apr-iconv':
        self.external_project = cfg.apr_iconv_path \
                                + self.external_project[9:]
      elif self.external_project[:8] == 'apr-util':
        self.external_project = cfg.apr_util_path \
                                + self.external_project[8:]
      elif self.external_project[:3] == 'apr':
        self.external_project = cfg.apr_path \
                                + self.external_project[3:]

  def add_dependencies(self, graph, cfg, extmap):
    if self.external_lib or self.external_project:
      if self.external_project:
        graph.add(DT_LIST, LT_PROJECT, self)
      return

    # the specified install area depends upon this target
    graph.add(DT_INSTALL, self.install, self)

    sources = _collect_paths(self.sources or '*.c', self.path)
    sources.sort()

    for src, reldir in sources:
      if src[-2:] == '.c':
        objname = src[:-2] + self.objext
      elif src[-4:] == '.cpp':
        objname = src[:-4] + self.objext
      else:
        raise GenError('ERROR: unknown file extension on ' + src)

      ofile = ObjectFile(objname, self.compile_cmd)

      # object depends upon source
      graph.add(DT_OBJECT, ofile, SourceFile(src, reldir))

      # target (a linked item) depends upon object
      graph.add(DT_LINK, self.name, ofile)

    # collect all the paths where stuff might get built
    ### we should collect this from the dependency nodes rather than
    ### the sources. "what dir are you going to put yourself into?"
    graph.add(DT_LIST, LT_TARGET_DIRS, self.path)
    for pattern in string.split(self.sources):
      idx = string.rfind(pattern, '/')
      if idx != -1:
        ### hmm. probably shouldn't be os.path.join() right here
        ### (at this point in the control flow; defer to output)
        graph.add(DT_LIST, LT_TARGET_DIRS, os.path.join(self.path,
                                                        pattern[:idx]))

class TargetExe(TargetLinked):
  def __init__(self, name, options, cfg, extmap):
    TargetLinked.__init__(self, name, options, cfg, extmap)

    self.objext = extmap['exe', 'object']
    self.filename = os.path.join(self.path, name + extmap['exe', 'target'])

    self.manpages = options.get('manpages', '')
    self.testing = options.get('testing')

  def add_dependencies(self, graph, cfg, extmap):
    TargetLinked.add_dependencies(self, graph, cfg, extmap)

    # collect test programs
    if self.install == 'test':
      graph.add(DT_LIST, LT_TEST_DEPS, self.filename)
      if self.testing != 'skip':
        graph.add(DT_LIST, LT_TEST_PROGS, self.filename)
    elif self.install == 'fs-test':
      graph.add(DT_LIST, LT_FS_TEST_DEPS, self.filename)
      if self.testing != 'skip':
        graph.add(DT_LIST, LT_FS_TEST_PROGS, self.filename)

    graph.bulk_add(DT_LIST, LT_MANPAGES, string.split(self.manpages))

class TargetScript(Target):
  def add_dependencies(self, graph, cfg, extmap):
    # we don't need to "compile" the sources, so there are no dependencies
    # to add here, except to get the script installed in the proper area.
    # note that the script might itself be generated, but that isn't a
    # concern here.
    graph.add(DT_INSTALL, self.install, self)

class TargetLib(TargetLinked):
  def __init__(self, name, options, cfg, extmap):
    TargetLinked.__init__(self, name, options, cfg, extmap)

    self.objext = extmap['lib', 'object']

    # the target file is the name, version, and appropriate extension
    tfile = '%s-%s%s' % (name, cfg.version, extmap['lib', 'target'])
    self.filename = os.path.join(self.path, tfile)

    self.msvc_static = options.get('msvc-static') == 'yes' # is a static lib
    self.msvc_fake = options.get('msvc-fake') == 'yes' # has fake target

class TargetApacheMod(TargetLib):

  def __init__(self, name, options, cfg, extmap):
    TargetLib.__init__(self, name, options, cfg, extmap)

    tfile = name + extmap['lib', 'target']
    self.filename = os.path.join(self.path, tfile)

    # we have a custom linking rule
    ### hmm. this is Makefile-specific
    self.compile_cmd = '$(COMPILE_APACHE_MOD)'
    self.link_cmd = '$(LINK_APACHE_MOD)'

class TargetRaModule(TargetLib):
  pass

class TargetDoc(Target):
  pass

class TargetSWIG(TargetLib):
  def __init__(self, name, options, cfg, extmap, lang):
    TargetLib.__init__(self, name, options, cfg, extmap)
    self.lang = lang
    self.desc = self.desc + ' for ' + lang_full_name[lang]
    self.shared_dir = 1

    ### hmm. this is Makefile-specific
    self.link_cmd = '$(LINK_%s_WRAPPER)' % string.upper(lang_abbrev[lang])

  def add_dependencies(self, graph, cfg, extmap):
    sources = _collect_paths(self.sources, self.path)
    assert len(sources) == 1  ### simple assertions for now

    # get path to SWIG .i file
    ipath = sources[0][0]
    iname = os.path.split(ipath)[1]

    assert iname[-2:] == '.i'
    cname = iname[:-2] + '.c'
    oname = iname[:-2] + extmap['lib', 'object']

    ### we should really extract the %module line
    libname = iname[:4] != 'svn_' and ('_' + iname[:-2]) or iname[3:-2]
    libfile = libname + extmap['lib', 'target']

    self.name = self.lang + libname
    self.path = os.path.join(self.path, self.lang)
    self.filename = os.path.join(self.path, libfile)

    ifile = SWIGSource(ipath)
    cfile = SWIGObject(os.path.join(self.path, cname), self.lang)
    ofile = SWIGObject(os.path.join(self.path, oname), self.lang)

    # the .c file depends upon the .i file
    graph.add(DT_SWIG_C, cfile, ifile)

    # the object depends upon the .c file
    graph.add(DT_OBJECT, ofile, cfile)

    # the library depends upon the object
    graph.add(DT_LINK, self.name, ofile)

    abbrev = lang_abbrev[self.lang]

    # the specified install area depends upon the library
    graph.add(DT_INSTALL, 'swig-' + abbrev, self)

  class Section(TargetLib.Section):
    def create_targets(self, graph, name, cfg, extmap):
      self.targets = { }
      for lang in cfg.swig_lang:
        target = self.target_class(name, self.options, cfg, extmap, lang)
        target.add_dependencies(graph, cfg, extmap)
        self.targets[lang] = target

    def get_targets(self):
      return self.targets.values()

    def get_dep_targets(self, target):
      target = self.targets.get(target.lang, None)
      return target and [target] or [ ]

class TargetSWIGRuntime(TargetSWIG):
  def add_dependencies(self, graph, cfg, extmap):
    abbrev = lang_abbrev[self.lang]
    name = 'swig' + abbrev
    cname = name + '.c'
    oname = name + extmap['lib', 'object']
    libname = name + extmap['lib', 'target']

    self.name = self.lang + '_runtime' 
    self.path = os.path.join(self.path, self.lang)
    self.filename = os.path.join(self.path, libname)
    self.external_lib = '-lswig' + abbrev

    cfile = SWIGObject(os.path.join(self.path, cname), self.lang)
    ofile = SWIGObject(os.path.join(self.path, oname), self.lang)
    graph.add(DT_OBJECT, ofile, cfile)
    graph.add(DT_LINK, self.name, ofile)
    graph.add(DT_INSTALL, 'swig_runtime-' + abbrev, self)

  class Section(TargetSWIG.Section):
    def create_targets(self, graph, name, cfg, extmap):
      self.targets = { }
      for lang in cfg.swig_lang:
        if lang == 'java':
          # java doesn't seem to have a separate runtime  
          continue      
        target = self.target_class(name, self.options, cfg, extmap, lang)
        target.add_dependencies(graph, cfg, extmap)
        self.targets[lang] = target

class TargetSWIGLib(TargetLib):
  def __init__(self, name, options, cfg, extmap):
    TargetLib.__init__(self, name, options, cfg, extmap)
    self.lang = options.get('lang')

  class Section(TargetLib.Section):
    def get_dep_targets(self, target):
      if target.lang == self.target.lang:
        return [ self.target ]
      return [ ]

class TargetProject(Target):
  def __init__(self, name, options, cfg, extmap):
    Target.__init__(self, name, options, cfg, extmap)
    self.cmd = options.get('cmd')
    self.release = options.get('release')
    self.debug = options.get('debug')
    self.filename = name

  def add_dependencies(self, graph, cfg, extmap):
    graph.add(DT_LIST, LT_PROJECT, self)

class TargetSWIGProject(TargetProject):
  def __init__(self, name, options, cfg, extmap):
    TargetProject.__init__(self, name, options, cfg, extmap)
    self.lang = options.get('lang')

class TargetJava(TargetLib):
  def __init__(self, name, options, cfg, extmap):
    TargetLib.__init__(self, name, options, cfg, extmap)
    self.link_cmd = options.get('link-cmd')
    self.packages = string.split(options.get('package-roots', ''))
    self.jar = options.get('jar')
    self.deps = [ ]
    self.filename = ''

class TargetJavaHeaders(TargetJava):
  def __init__(self, name, options, cfg, extmap):
    TargetJava.__init__(self, name, options, cfg, extmap)
    self.objext = '.class'
    self.javah_objext = '.h'
    self.headers = options.get('headers')
    self.classes = options.get('classes')
    self.package = options.get('package')
    self.extra_classes = string.split(options.get('extra-classes'))
    self.output_dir = self.headers

  def add_dependencies(self, graph, cfg, extmap):
    sources = _collect_paths(self.sources, self.path)

    for src, reldir in sources:
      if src[-5:] != '.java':
        raise GenError('ERROR: unknown file extension on ' + src)

      class_path = os.path.split(src[:-5])

      class_header = os.path.join(self.headers, class_path[1] + '.h')
      class_pkg_list = string.split(self.package, '.')
      class_pkg = ''
      for dir in class_pkg_list:
        class_pkg = os.path.join(class_pkg, dir)
      class_file = ObjectFile(os.path.join(self.classes, class_pkg,
                                            class_path[1] + self.objext))
      class_file.source_generated = 1
      hfile = HeaderFile(class_header, self.package + '.' + class_path[1],
                         self.compile_cmd)
      hfile.source_generated = 1
      graph.add(DT_OBJECT, hfile, class_file)
      self.deps.append(hfile)

      # target (a linked item) depends upon object
      graph.add(DT_LINK, self.name, hfile)

    for extra in self.extra_classes:
      hfile = HeaderFile('', self.package + '.' + extra)
      hfile.source_generated = 1
      self.deps.append(hfile)

    # collect all the paths where stuff might get built
    ### we should collect this from the dependency nodes rather than
    ### the sources. "what dir are you going to put yourself into?"
    graph.add(DT_LIST, LT_TARGET_DIRS, self.path)
    graph.add(DT_LIST, LT_TARGET_DIRS, self.classes)
    graph.add(DT_LIST, LT_TARGET_DIRS, self.headers)
    for pattern in string.split(self.sources):
      idx = string.rfind(pattern, '/')
      if idx != -1:
        ### hmm. probably shouldn't be os.path.join() right here
        ### (at this point in the control flow; defer to output)
        graph.add(DT_LIST, LT_TARGET_DIRS, os.path.join(self.path,
                                                        pattern[:idx]))

    graph.add(DT_INSTALL, self.name, self)


  def get_dep_targets(self, target):
    return [ self.target ]

class TargetJavaClasses(TargetJava):
  def __init__(self, name, options, cfg, extmap):
    TargetJava.__init__(self, name, options, cfg, extmap)
    self.objext = '.class'
    self.lang = 'java'
    self.classes = options.get('classes')
    self.output_dir = self.classes

  def add_dependencies(self, graph, cfg, extmap):
    ### FIXME: SWIG/Java's generated .java source directory and files
    ### are created by the build process, and are not available when
    ### this code is run.  This HACK supresses the GenError thrown by
    ### the _collect_paths() function.
    sources = None
    try:
      sources =_collect_paths(self.sources, self.path)
    except GenError:
      sources = ()

    for src, reldir in sources:
      if src[-5:] == '.java':
        objname = src[:-5] + self.objext

        # As .class files are likely not generated into the same
        # directory as the source files, the object path may need
        # adjustment.  To this effect, take "target_ob.classes" into
        # account.
        dirs = string.split(objname, '/')
        i = len(dirs) - 2  # Last element is the .class file name.
        while i >= 0:
          if dirs[i] in self.packages:
            # Java package root found.
            objname = os.path.join(self.classes, string.join(dirs[i:], os.sep))
            break
          i = i - 1
        if i < 0:
              raise GenError('Unable to find Java package root in path')
      else:
        raise GenError('ERROR: unknown file extension on ' + src)

      ofile = ObjectFile(objname, self.compile_cmd)
      sfile = SourceFile(src, reldir)

      # object depends upon source
      graph.add(DT_OBJECT, ofile, sfile)
      self.deps.append(sfile)

      # target (a linked item) depends upon object
      graph.add(DT_LINK, self.name, ofile)

    # collect all the paths where stuff might get built
    ### we should collect this from the dependency nodes rather than
    ### the sources. "what dir are you going to put yourself into?"
    graph.add(DT_LIST, LT_TARGET_DIRS, self.path)
    graph.add(DT_LIST, LT_TARGET_DIRS, self.classes)
    for pattern in string.split(self.sources):
      idx = string.rfind(pattern, '/')
      if idx != -1:
        ### hmm. probably shouldn't be os.path.join() right here
        ### (at this point in the control flow; defer to output)
        graph.add(DT_LIST, LT_TARGET_DIRS, os.path.join(self.path,
                                                        pattern[:idx]))

    graph.add(DT_INSTALL, self.name, self)

  class Section(TargetLib.Section):
    def create_targets(self, graph, name, cfg, extmap):
      self.targets = { }
      target = self.target_class(name, self.options, cfg, extmap)
      target.add_dependencies(graph, cfg, extmap)
      self.targets = target

    def get_targets(self):
      return self.targets.values()

    def get_dep_targets(self, target):
      target = self.targets
      return target and [target] or [ ]


_build_types = {
  'exe' : TargetExe,
  'script' : TargetScript,
  'lib' : TargetLib,
  'doc' : TargetDoc,
  'swig' : TargetSWIG,
  'project' : TargetProject,
  'swig_runtime' : TargetSWIGRuntime,
  'swig_lib' : TargetSWIGLib,
  'swig_project' : TargetSWIGProject,
  'ra-module': TargetRaModule,
  'apache-mod': TargetApacheMod,
  'javah' : TargetJavaHeaders,
  'java' : TargetJavaClasses,
  }


class Config:
  pass


class GenError(Exception):
  pass


_predef_sections = [
  'options',
  'static-apache',
  'test-scripts',
  'fs-test-scripts',
  'swig-dirs',
  ]

def _filter_sections(t):
  """Sort list of section names and remove predefined sections"""
  t = t[:]
  for s in _predef_sections:
    if s in t:
      t.remove(s)
  t.sort()
  return t

def _collect_paths(pats, path=None):
  """Find files matching a space separated list of globs
  
  pats (string) is the list of glob patterns

  path (string), if specified, is a path that will be prepended to each
    glob pattern before it is evaluated
    
  If path is none the return value is a list of filenames, otherwise
  the return value is a list of 2-tuples. The first element in each tuple
  is a matching filename and the second element is the portion of the
  glob pattern which matched the file before its last forward slash (/)
  """
  result = [ ]
  for base_pat in string.split(pats):
    if path:
      ### these paths are actually '/'-based
      pattern = os.path.join(path, base_pat)
    else:
      pattern = base_pat
    files = glob.glob(pattern)
    if not files:
      raise GenError('ERROR: "%s" found no files.' % pattern)

    if path is None:
      # just append the names to the result list
      result.extend(files)
    else:
      # if we have paths, then we need to record how each source is located
      # relative to the specified path
      idx = string.rfind(base_pat, '/')
      if idx == -1:
        reldir = ''
      else:
        reldir = base_pat[:idx]
        assert not glob.has_magic(reldir)
      for file in files:
        result.append((file, reldir))

  return result

def _strip_path(path, files):
  "Strip the given path from each file."
  if path[-1] not in (os.sep, os.altsep):
    path = path + os.sep
  l = len(path)
  result = [ ]
  for file in files:
    assert file[:l] == path
    result.append(file[l:])
  return result

def _retreat_dots(path):
  "Given a relative directory, return ../ paths to retreat to the origin."
  parts = string.split(path, os.sep)
  return (os.pardir + os.sep) * len(parts)

def _find_includes(fname, include_deps):
  """Return list of files in include_deps included by fname"""
  hdrs = _scan_for_includes(fname, include_deps.keys())
  return _include_closure(hdrs, include_deps).keys()

def _create_include_deps(includes, prev_deps={}):
  """Find files included by a list of files
  
  includes (sequence of strings) is a list of files which should
    be scanned for includes
    
  prev_deps (dictionary) is an optional parameter which may contain
    the return value of a previous call to _create_include_deps. All
    data inside will be included in the return value of the current
    call.
    
  Return value is a dictionary with one entry for each file that
    was scanned (in addition the entries from prev_deps). The key
    for an entry is the short file name of the file that was scanned
    and the value is a 2-tuple containing the long file name and a
    dictionary of files included by that file.
  """
  
  shorts = map(os.path.basename, includes)

  # limit intra-header dependencies to just these headers, and what we
  # may have found before
  limit = shorts + prev_deps.keys()

  deps = prev_deps.copy()
  for inc in includes:
    short = os.path.basename(inc)
    deps[short] = (inc, _scan_for_includes(inc, limit))

  # keep recomputing closures until we see no more changes
  while 1:
    changes = 0
    for short in shorts:
      old = deps[short]
      deps[short] = (old[0], _include_closure(old[1], deps))
      if not changes:
        ok = old[1].keys()
        ok.sort()
        nk = deps[short][1].keys()
        nk.sort()
        changes = ok != nk
    if not changes:
      return deps

def _include_closure(hdrs, deps):
  """Update a set of dependencies with dependencies of dependencies
  
  hdrs (dictionary) is a set of dependencies. It is a dictionary with
    filenames as keys and None as values
    
  deps (dictionary) is a big catalog of dependencies in the format
    returned by _create_include_deps.
    
  Return value is a copy of the hdrs dictionary updated with new
    entries for files that the existing entries include, according
    to the information in deps.
  """

  new = hdrs.copy()
  for h in hdrs.keys():
    new.update(deps[h][1])
  return new

_re_include = re.compile(r'^#\s*include\s*[<"]([^<"]+)[>"]')
def _scan_for_includes(fname, limit):
  """Find headers directly included by a C source file.
  
  fname (string) is the name of the file to scan
  
  limit (sequence or dictionary) is a collection of file names
    which may be included. Included files which aren't found
    in this collection will be ignored.
  
  Return value is a dictionary with included file names as keys and
  None as values.
  """
  # note: we don't worry about duplicates in the return list
  hdrs = { }
  for line in fileinput.input(fname):
    match = _re_include.match(line)
    if match:
      h = match.group(1)
      if h in limit:
        hdrs[h] = None
  return hdrs

def _sorted_files(graph, area):
  "Given a list of targets, sort them based on their dependencies."

  # we're going to just go with a naive algorithm here. these lists are
  # going to be so short, that we can use O(n^2) or whatever this is.

  inst_targets = graph.get_sources(DT_INSTALL, area)

  # first we need our own copy of the target list since we're going to
  # munge it.
  targets = inst_targets[:]

  # the output list of the targets' files
  files = [ ]

  # loop while we have targets remaining:
  while targets:
    # find a target that has no dependencies in our current targets list.
    for t in targets:
      s = graph.get_sources(DT_LINK, t.name, Target) \
          + graph.get_sources(DT_NONLIB, t.name, Target)
      for d in s:
        if d in targets:
          break
      else:
        # no dependencies found in the targets list. this is a good "base"
        # to add to the files list now.
        files.append(t.filename)

        # don't consider this target any more
        targets.remove(t)

        # break out of search through targets
        break
    else:
      # we went through the entire target list and everything had at least
      # one dependency on another target. thus, we have a circular dependency
      # tree. somebody messed up the .conf file, or the app truly does have
      # a loop (and if so, they're screwed; libtool can't relink a lib at
      # install time if the dependent libs haven't been installed yet)
      raise CircularDependencies()

  return files

class CircularDependencies(Exception):
  pass

def unique(seq):
  "Eliminate duplicates from a sequence"
  list = [ ]
  dupes = { }
  for e in seq:
    if not dupes.has_key(e):
      dupes[e] = None
      list.append(e)
  return list

### End of file.

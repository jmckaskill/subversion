#
# gen_dsp.py -- generate Microsoft Visual C++ 6 projects
#

import os
import sys
import string

import gen_base
import gen_win
import ezt


class Generator(gen_win.WinGeneratorBase):
  "Generate a Microsoft Visual C++ 6 project"

  def __init__(self, fname, verfname, options):
    gen_win.WinGeneratorBase.__init__(self, fname, verfname, options,
                                      'msvc-dsp')

  def default_output(self, conf_path):
    return 'subversion_msvc.dsw'

  def quote(self, str):
    return '"%s"' % str

  def write_project(self, target, fname, rootpath):
    "Write a Project (.dsp)"

    if isinstance(target, gen_base.TargetExe):
      targtype = "Win32 (x86) Console Application"
      targval = "0x0103"
      target.output_name = target.name + '.exe'
    elif isinstance(target, gen_base.TargetJava):
        targtype = "Win32 (x86) Generic Project"
        targval = "0x010a"
        target.output_name = None
    elif isinstance(target, gen_base.TargetLib):
      if target.msvc_static:
        targtype = "Win32 (x86) Static Library"
        targval = "0x0104"
        target.output_name = '%s-%d.lib' % (target.name, self.cfg.version)
      else:
        targtype = "Win32 (x86) Dynamic-Link Library"
        targval = "0x0102"
        target.output_name = os.path.basename(target.filename)
    elif isinstance(target, gen_base.TargetProject):
      if target.cmd:
        targtype = "Win32 (x86) External Target"
        targval = "0x0106"
      else:
        targtype = "Win32 (x86) Generic Project"
        targval = "0x010a"
    else:
      raise gen_base.GenError("Cannot create project for %s" % target.name)

    if isinstance(target, gen_base.TargetApacheMod):
      target.output_name = target.name + '.so'

    configs = self.get_configs(target, rootpath)

    sources = self.get_proj_sources(True, target, rootpath)

    data = {
      'target' : target,
      'target_type' : targtype,
      'target_number' : targval,
      'rootpath' : rootpath,
      'platforms' : self.platforms,
      'configs' : configs,
      'includes' : self.get_win_includes(target, rootpath),
      'sources' : sources,
      'default_platform' : self.platforms[0],
      'default_config' : configs[0].name,
      'is_exe' : ezt.boolean(isinstance(target, gen_base.TargetExe)),
      'is_external' : ezt.boolean((isinstance(target, gen_base.TargetProject)
                                   or isinstance(target, gen_base.TargetI18N))
                                  and target.cmd),
      'is_utility' : ezt.boolean(isinstance(target,
                                            gen_base.TargetProject)),
      'is_dll' : ezt.boolean(isinstance(target, gen_base.TargetLib)
                             and not target.msvc_static),
      'instrument_apr_pools' : self.instrument_apr_pools,
      'instrument_purify_quantify' : self.instrument_purify_quantify,
      }

    self.write_with_template(fname, 'msvc_dsp.ezt', data)

  def write(self, oname):
    "Write a Workspace (.dsw)"

    install_targets = self.get_install_targets()
    
    targets = [ ]

    self.gen_proj_names(install_targets)

    # Traverse the targets and generate the project files
    for target in install_targets:
      name = target.name
      if ((isinstance(target, gen_base.TargetLinked)
         or isinstance(target, gen_base.TargetI18N))
         and target.external_project):
        # Figure out where the external .dsp is located.
        fname = target.external_project + '.dsp'
      else:
        fname = os.path.join(self.projfilesdir,
                             "%s_msvc.dsp" % target.proj_name)
        depth = string.count(self.projfilesdir, os.sep) + 1
        self.write_project(target, fname, string.join(['..']*depth, '\\'))

      if '-' in fname:
        fname = '"%s"' % fname
        
      depends = [ ]
      if not isinstance(target, gen_base.TargetI18N):
        depends = self.adjust_win_depends(target, name)
	#print name
	#for dep in depends:
	#  print "	",dep.name

      dep_names = [ ]
      for dep in depends:
        dep_names.append(dep.proj_name)

      targets.append(
        gen_win.ProjectItem(name=target.proj_name,
                            dsp=string.replace(fname, os.sep, '\\'),
                            depends=dep_names))

    targets.sort(lambda x, y: cmp(x.name, y.name))
    data = {
      'targets' : targets,
      }

    self.write_with_template(oname, 'msvc_dsw.ezt', data)


# compatibility with older Pythons:
try:
  True
except NameError:
  True = 1
  False = 0

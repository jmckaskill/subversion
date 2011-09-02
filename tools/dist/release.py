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


# About this script:
#   This script is intended to simplify creating Subversion releases, by
#   automating as much as is possible.  It works well with our Apache
#   infrastructure, and should make rolling, posting, and announcing
#   releases dirt simple.
#
#   This script may be run on a number of platforms, but it is intended to
#   be run on people.apache.org.  As such, it may have dependencies (such
#   as Python version) which may not be common, but are guaranteed to be
#   available on people.apache.org.

# It'd be kind of nice to use the Subversion python bindings in this script,
# but people.apache.org doesn't currently have them installed

# Stuff we need
import os
import re
import sys
import glob
import shutil
import urllib2
import hashlib
import tarfile
import logging
import datetime
import tempfile
import operator
import itertools
import subprocess
import argparse       # standard in Python 2.7

# Find ezt, using Subversion's copy, if there isn't one on the system.
try:
    import ezt
except ImportError:
    ezt_path = os.path.dirname(os.path.dirname(os.path.abspath(sys.path[0])))
    ezt_path = os.path.join(ezt_path, 'build', 'generator')
    sys.path.append(ezt_path)

    import ezt


# Our required / recommended versions
autoconf_ver = '2.68'
libtool_ver = '2.4'
swig_ver = '2.0.4'

# Some constants
repos = 'http://svn.apache.org/repos/asf/subversion'
people_host = 'minotaur.apache.org'
people_dist_dir = '/www/www.apache.org/dist/subversion'


#----------------------------------------------------------------------
# Utility functions

class Version(object):
    regex = re.compile('(\d+).(\d+).(\d+)(?:-(?:(rc|alpha|beta)(\d+)))?')

    def __init__(self, ver_str):
        # Special case the 'trunk-nightly' version
        if ver_str == 'trunk-nightly':
            self.major = None
            self.minor = None
            self.patch = None
            self.pre = 'nightly'
            self.pre_num = None
            self.base = 'nightly'
            return

        match = self.regex.search(ver_str)

        if not match:
            raise RuntimeError("Bad version string '%s'" % ver_str)

        self.major = int(match.group(1))
        self.minor = int(match.group(2))
        self.patch = int(match.group(3))

        if match.group(4):
            self.pre = match.group(4)
            self.pre_num = int(match.group(5))
        else:
            self.pre = None
            self.pre_num = None

        self.base = '%d.%d.%d' % (self.major, self.minor, self.patch)

    def is_prerelease(self):
        return self.pre != None

    def __lt__(self, that):
        if self.major < that.major: return True
        if self.major > that.major: return False

        if self.minor < that.minor: return True
        if self.minor > that.minor: return False

        if self.patch < that.patch: return True
        if self.patch > that.patch: return False

        if not self.pre and not that.pre: return False
        if not self.pre and that.pre: return False
        if self.pre and not that.pre: return True

        # We are both pre-releases
        if self.pre != that.pre:
            return self.pre < that.pre
        else:
            return self.pre_num < that.pre_num

    def __str(self):
        if self.pre:
            if self.pre == 'nightly':
                return 'nightly'
            else:
                extra = '-%s%d' % (self.pre, self.pre_num)
        else:
            extra = ''

        return self.base + extra

    def __repr__(self):

        return "Version('%s')" % self.__str()

    def __str__(self):
        return self.__str()


def get_prefix(base_dir):
    return os.path.join(base_dir, 'prefix')

def get_tempdir(base_dir):
    return os.path.join(base_dir, 'tempdir')

def get_deploydir(base_dir):
    return os.path.join(base_dir, 'deploy')

def get_tmpldir():
    return os.path.join(os.path.abspath(sys.path[0]), 'templates')

def get_tmplfile(filename):
    try:
        return open(os.path.join(get_tmpldir(), filename))
    except IOError:
        # Hmm, we had a problem with the local version, let's try the repo
        return urllib2.urlopen(repos + '/trunk/tools/dist/templates/' + filename)

def get_nullfile():
    # This is certainly not cross platform
    return open('/dev/null', 'w')

def run_script(verbose, script):
    if verbose:
        stdout = None
        stderr = None
    else:
        stdout = get_nullfile()
        stderr = subprocess.STDOUT

    for l in script.split('\n'):
        subprocess.check_call(l.split(), stdout=stdout, stderr=stderr)

def download_file(url, target):
    response = urllib2.urlopen(url)
    target_file = open(target, 'w')
    target_file.write(response.read())

def assert_people():
    if os.uname()[1] != people_host:
        raise RuntimeError('Not running on expected host "%s"' % people_host)

#----------------------------------------------------------------------
# Cleaning up the environment

def cleanup(args):
    'Remove generated files and folders.'
    logging.info('Cleaning')

    shutil.rmtree(get_prefix(args.base_dir), True)
    shutil.rmtree(get_tempdir(args.base_dir), True)
    shutil.rmtree(get_deploydir(args.base_dir), True)


#----------------------------------------------------------------------
# Creating an environment to roll the release

class RollDep(object):
    'The super class for each of the build dependencies.'
    def __init__(self, base_dir, use_existing, verbose):
        self._base_dir = base_dir
        self._use_existing = use_existing
        self._verbose = verbose

    def _test_version(self, cmd):
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (stdout, stderr) = proc.communicate()
        rc = proc.wait()
        if rc: return ''

        return stdout.split('\n')

    def build(self):
        if not hasattr(self, '_extra_configure_flags'):
            self._extra_configure_flags = ''
        cwd = os.getcwd()
        tempdir = get_tempdir(self._base_dir)
        tarball = os.path.join(tempdir, self._filebase + '.tar.gz')

        if os.path.exists(tarball):
            if not self._use_existing:
                raise RuntimeError('autoconf tarball "%s" already exists'
                                                                    % tarball)
            logging.info('Using existing %s.tar.gz' % self._filebase)
        else:
            logging.info('Fetching %s' % self._filebase)
            download_file(self._url, tarball)

        # Extract tarball
        tarfile.open(tarball).extractall(tempdir)

        logging.info('Building ' + self.label)
        os.chdir(os.path.join(tempdir, self._filebase))
        run_script(self._verbose,
                   '''./configure --prefix=%s %s
                      make
                      make install''' % (get_prefix(self._base_dir),
                                         self._extra_configure_flags))

        os.chdir(cwd)


class AutoconfDep(RollDep):
    def __init__(self, base_dir, use_existing, verbose):
        RollDep.__init__(self, base_dir, use_existing, verbose)
        self.label = 'autoconf'
        self._filebase = 'autoconf-' + autoconf_ver
        self._url = 'http://ftp.gnu.org/gnu/autoconf/%s.tar.gz' % self._filebase

    def have_usable(self):
        output = self._test_version(['autoconf', '-V'])
        if not output: return False

        version = output[0].split()[-1:][0]
        return version == autoconf_ver

    def use_system(self):
        if not self._use_existing: return False
        return self.have_usable()


class LibtoolDep(RollDep):
    def __init__(self, base_dir, use_existing, verbose):
        RollDep.__init__(self, base_dir, use_existing, verbose)
        self.label = 'libtool'
        self._filebase = 'libtool-' + libtool_ver
        self._url = 'http://ftp.gnu.org/gnu/libtool/%s.tar.gz' % self._filebase

    def have_usable(self):
        output = self._test_version(['libtool', '--version'])
        if not output: return False

        version = output[0].split()[-1:][0]
        return version == libtool_ver

    def use_system(self):
        # We unconditionally return False here, to avoid using a borked
        # system libtool (I'm looking at you, Debian).
        return False


class SwigDep(RollDep):
    def __init__(self, base_dir, use_existing, verbose, sf_mirror):
        RollDep.__init__(self, base_dir, use_existing, verbose)
        self.label = 'swig'
        self._filebase = 'swig-' + swig_ver
        self._url = 'http://sourceforge.net/projects/swig/files/swig/%(swig)s/%(swig)s.tar.gz/download?use_mirror=%(sf_mirror)s' % \
            { 'swig' : self._filebase,
              'sf_mirror' : sf_mirror }
        self._extra_configure_flags = '--without-pcre'

    def have_usable(self):
        output = self._test_version(['swig', '-version'])
        if not output: return False

        version = output[1].split()[-1:][0]
        return version == swig_ver

    def use_system(self):
        if not self._use_existing: return False
        return self.have_usable()


def build_env(args):
    'Download prerequisites for a release and prepare the environment.'
    logging.info('Creating release environment')

    try:
        os.mkdir(get_prefix(args.base_dir))
        os.mkdir(get_tempdir(args.base_dir))
    except OSError:
        if not args.use_existing:
            raise

    autoconf = AutoconfDep(args.base_dir, args.use_existing, args.verbose)
    libtool = LibtoolDep(args.base_dir, args.use_existing, args.verbose)
    swig = SwigDep(args.base_dir, args.use_existing, args.verbose,
                   args.sf_mirror)

    # iterate over our rolling deps, and build them if needed
    for dep in [autoconf, libtool, swig]:
        if dep.use_system():
            logging.info('Using system %s' % dep.label)
        else:
            dep.build()


#----------------------------------------------------------------------
# Create release artifacts

def fetch_changes(repos, branch, revision):
    changes_peg_url = '%s/%s/CHANGES@%d' % (repos, branch, revision)
    proc = subprocess.Popen(['svn', 'cat', changes_peg_url],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (stdout, stderr) = proc.communicate()
    proc.wait()
    return stdout.split('\n')


def compare_changes(repos, branch, revision):
    # Compare trunk's version of CHANGES with that of the branch,
    # ignoring any lines in trunk's version precede what *should*
    # match the contents of the branch's version.  (This allows us to
    # continue adding new stuff at the top of trunk's CHANGES that
    # might relate to the *next* major release line.)
    branch_CHANGES = fetch_changes(repos, branch, revision)
    trunk_CHANGES = fetch_changes(repos, 'trunk', revision)
    try:
        first_matching_line = trunk_CHANGES.index(branch_CHANGES[0])
    except ValueError:
        raise RuntimeError('CHANGES not synced between trunk and branch')

    trunk_CHANGES = trunk_CHANGES[first_matching_line:]
    saw_diff = False
    import difflib
    for diff_line in difflib.unified_diff(trunk_CHANGES, branch_CHANGES):
        saw_diff = True
        logging.debug('%s', diff_line)
    if saw_diff:
        raise RuntimeError('CHANGES not synced between trunk and branch')


def roll_tarballs(args):
    'Create the release artifacts.'
    extns = ['zip', 'tar.gz', 'tar.bz2']

    if args.branch:
        branch = args.branch
    else:
        branch = 'branches/' + args.version.base[:-1] + 'x'

    logging.info('Rolling release %s from branch %s@%d' % (args.version,
                                                           branch, args.revnum))

    # Ensure we've got the appropriate rolling dependencies available
    autoconf = AutoconfDep(args.base_dir, False, args.verbose)
    libtool = LibtoolDep(args.base_dir, False, args.verbose)
    swig = SwigDep(args.base_dir, False, args.verbose, None)

    for dep in [autoconf, libtool, swig]:
        if not dep.have_usable():
           raise RuntimeError('Cannot find usable %s' % dep.label)

    if branch != 'trunk':
        # Make sure CHANGES is sync'd.    
        compare_changes(repos, branch, args.revnum)
    
    # Create the output directory
    if not os.path.exists(get_deploydir(args.base_dir)):
        os.mkdir(get_deploydir(args.base_dir))

    # For now, just delegate to dist.sh to create the actual artifacts
    extra_args = ''
    if args.version.is_prerelease():
        if args.version.pre == 'nightly':
            extra_args = '-nightly'
        else:
            extra_args = '-%s %d' % (args.version.pre, args.version.pre_num)
    logging.info('Building UNIX tarballs')
    run_script(args.verbose, '%s/dist.sh -v %s -pr %s -r %d %s'
                     % (sys.path[0], args.version.base, branch, args.revnum,
                        extra_args) )
    logging.info('Buildling Windows tarballs')
    run_script(args.verbose, '%s/dist.sh -v %s -pr %s -r %d -zip %s'
                     % (sys.path[0], args.version.base, branch, args.revnum,
                        extra_args) )

    # Move the results to the deploy directory
    logging.info('Moving artifacts and calculating checksums')
    for e in extns:
        if args.version.pre == 'nightly':
            filename = 'subversion-nightly.%s' % e
        else:
            filename = 'subversion-%s.%s' % (args.version, e)

        shutil.move(filename, get_deploydir(args.base_dir))
        filename = os.path.join(get_deploydir(args.base_dir), filename)
        m = hashlib.sha1()
        m.update(open(filename, 'r').read())
        open(filename + '.sha1', 'w').write(m.hexdigest())

    shutil.move('svn_version.h.dist', get_deploydir(args.base_dir))

    # And we're done!


#----------------------------------------------------------------------
# Post the candidate release artifacts

def post_candidates(args):
    'Post the generated tarballs to web-accessible directory.'
    if args.target:
        target = args.target
    else:
        target = os.path.join(os.getenv('HOME'), 'public_html', 'svn',
                              str(args.version))

    if args.code_name:
        dirname = args.code_name
    else:
        dirname = 'deploy'

    if not os.path.exists(target):
        os.makedirs(target)

    data = { 'version'      : str(args.version),
             'revnum'       : args.revnum,
             'dirname'      : dirname,
           }

    # Choose the right template text
    if args.version.is_prerelease():
        if args.version.pre == 'nightly':
            template_filename = 'nightly-candidates.ezt'
        else:
            template_filename = 'rc-candidates.ezt'
    else:
        template_filename = 'stable-candidates.ezt'

    template = ezt.Template()
    template.parse(get_tmplfile(template_filename).read())
    template.generate(open(os.path.join(target, 'index.html'), 'w'), data)

    logging.info('Moving tarballs to %s' % os.path.join(target, dirname))
    if os.path.exists(os.path.join(target, dirname)):
        shutil.rmtree(os.path.join(target, dirname))
    shutil.copytree(get_deploydir(args.base_dir), os.path.join(target, dirname))


#----------------------------------------------------------------------
# Clean dist

def clean_dist(args):
    'Clean the distribution directory of all but the most recent artifacts.'

    regex = re.compile('subversion-(\d+).(\d+).(\d+)(?:-(?:(rc|alpha|beta)(\d+)))?')

    if not args.dist_dir:
        assert_people()
        args.dist_dir = people_dist_dir

    logging.info('Cleaning dist dir \'%s\'' % args.dist_dir)

    filenames = glob.glob(os.path.join(args.dist_dir, 'subversion-*.tar.gz'))
    versions = []
    for filename in filenames:
        versions.append(Version(filename))

    for k, g in itertools.groupby(sorted(versions),
                                  lambda x: (x.major, x.minor)):
        releases = list(g)
        logging.info("Saving release '%s'", releases[-1])

        for r in releases[:-1]:
            for filename in glob.glob(os.path.join(args.dist_dir,
                                                   'subversion-%s.*' % r)):
                logging.info("Removing '%s'" % filename)
                os.remove(filename)


#----------------------------------------------------------------------
# Move to dist

def move_to_dist(args):
    'Move candidate artifacts to the distribution directory.'

    if not args.dist_dir:
        assert_people()
        args.dist_dir = people_dist_dir

    if args.target:
        target = args.target
    else:
        target = os.path.join(os.getenv('HOME'), 'public_html', 'svn',
                              str(args.version), 'deploy')

    if args.code_name:
        dirname = args.code_name
    else:
        dirname = 'deploy'

    logging.info('Moving %s to dist dir \'%s\'' % (str(args.version),
                                                   args.dist_dir) )
    filenames = glob.glob(os.path.join(target,
                                       'subversion-%s.*' % str(args.version)))
    for filename in filenames:
        shutil.copy(filename, args.dist_dir)


#----------------------------------------------------------------------
# Write announcements

def write_news(args):
    'Write text for the Subversion website.'
    data = { 'date' : datetime.date.today().strftime('%Y%m%d'),
             'date_pres' : datetime.date.today().strftime('%Y-%m-%d'),
             'major-minor' : args.version.base[:3],
             'version' : str(args.version),
             'version_base' : args.version.base,
           }

    if args.version.is_prerelease():
        template_filename = 'rc-news.ezt'
    else:
        template_filename = 'stable-news.ezt'

    template = ezt.Template()
    template.parse(get_tmplfile(template_filename).read())
    template.generate(sys.stdout, data)


def get_sha1info(args):
    'Return a list of sha1 info for the release'
    sha1s = glob.glob(os.path.join(get_deploydir(args.base_dir), '*.sha1'))

    class info(object):
        pass

    sha1info = []
    for s in sha1s:
        i = info()
        i.filename = os.path.basename(s)[:-5]
        i.sha1 = open(s, 'r').read()
        sha1info.append(i)

    return sha1info


def write_announcement(args):
    'Write the release announcement.'
    sha1info = get_sha1info(args)

    data = { 'version'              : str(args.version),
             'sha1info'             : sha1info,
             'siginfo'              : open('getsigs-output', 'r').read(),
             'major-minor'          : args.version.base[:3],
             'major-minor-patch'    : args.version.base,
           }

    if args.version.is_prerelease():
        template_filename = 'rc-release-ann.ezt'
    else:
        template_filename = 'stable-release-ann.ezt'

    template = ezt.Template(compress_whitespace = False)
    template.parse(get_tmplfile(template_filename).read())
    template.generate(sys.stdout, data)


def write_downloads(args):
    'Output the download section of the website.'
    sha1info = get_sha1info(args)

    data = { 'version'              : str(args.version),
             'fileinfo'             : sha1info,
           }

    template = ezt.Template(compress_whitespace = False)
    template.parse(get_tmplfile('download.ezt').read())
    template.generate(sys.stdout, data)


#----------------------------------------------------------------------
# Validate the signatures for a release

key_start = '-----BEGIN PGP SIGNATURE-----\n'
fp_pattern = re.compile(r'^pub\s+(\w+\/\w+)[^\n]*\n\s+Key\sfingerprint\s=((\s+[0-9A-F]{4}){10})\nuid\s+([^<\(]+)\s')

def check_sigs(args):
    'Check the signatures for the release.'

    import gnupg
    gpg = gnupg.GPG()

    if args.target:
        target = args.target
    else:
        target = os.path.join(os.getenv('HOME'), 'public_html', 'svn',
                              str(args.version), 'deploy')

    good_sigs = {}

    for filename in glob.glob(os.path.join(target, 'subversion-*.asc')):
        text = open(filename).read()
        keys = text.split(key_start)

        for key in keys[1:]:
            fd, fn = tempfile.mkstemp()
            os.write(fd, key_start + key)
            os.close(fd)
            verified = gpg.verify_file(open(fn, 'rb'), filename[:-4])
            os.unlink(fn)

            if verified.valid:
                good_sigs[verified.key_id[-8:]] = True
            else:
                sys.stderr.write("BAD SIGNATURE for %s\n" % filename)
                sys.exit(1)

    for id in good_sigs.keys():
        gpg = subprocess.Popen(['gpg', '--fingerprint', id],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        rc = gpg.wait()
        gpg_output = gpg.stdout.read()
        if rc:
            print(gpg_output)
            sys.stderr.write("UNABLE TO GET FINGERPRINT FOR %s" % id)
            sys.exit(1)

        gpg_output = "\n".join([ l for l in gpg_output.splitlines()
                                                     if l[0:7] != 'Warning' ])

        fp = fp_pattern.match(gpg_output).groups()
        print("   %s [%s] with fingerprint:" % (fp[3], fp[0]))
        print("   %s" % fp[1])


#----------------------------------------------------------------------
# Main entry point for argument parsing and handling

def main():
    'Parse arguments, and drive the appropriate subcommand.'

    # Setup our main parser
    parser = argparse.ArgumentParser(
                            description='Create an Apache Subversion release.')
    parser.add_argument('--clean', action='store_true', default=False,
                   help='Remove any directories previously created by %(prog)s')
    parser.add_argument('--verbose', action='store_true', default=False,
                   help='Increase output verbosity')
    parser.add_argument('--base-dir', default=os.getcwd(),
                   help='''The directory in which to create needed files and
                           folders.  The default is the current working
                           directory.''')
    subparsers = parser.add_subparsers(title='subcommands')

    # Setup the parser for the build-env subcommand
    subparser = subparsers.add_parser('build-env',
                    help='''Download release prerequisistes, including autoconf,
                            libtool, and swig.''')
    subparser.set_defaults(func=build_env)
    subparser.add_argument('--sf-mirror', default='softlayer',
                    help='''The mirror to use for downloading files from
                            SourceForge.  If in the EU, you may want to use
                            'kent' for this value.''')
    subparser.add_argument('--use-existing', action='store_true', default=False,
                    help='''Attempt to use existing build dependencies before
                            downloading and building a private set.''')

    # Setup the parser for the roll subcommand
    subparser = subparsers.add_parser('roll',
                    help='''Create the release artifacts.''')
    subparser.set_defaults(func=roll_tarballs)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')
    subparser.add_argument('revnum', type=int,
                    help='''The revision number to base the release on.''')
    subparser.add_argument('--branch',
                    help='''The branch to base the release on.''')

    # Setup the parser for the post-candidates subcommand
    subparser = subparsers.add_parser('post-candidates',
                    help='''Build the website to host the candidate tarballs.
                            The default location is somewhere in ~/public_html.
                            ''')
    subparser.set_defaults(func=post_candidates)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')
    subparser.add_argument('revnum', type=int,
                    help='''The revision number to base the release on.''')
    subparser.add_argument('--target',
                    help='''The full path to the destination.''')
    subparser.add_argument('--code-name',
                    help='''A whimsical name for the release, used only for
                            naming the download directory.''')

    # The clean-dist subcommand
    subparser = subparsers.add_parser('clean-dist',
                    help='''Clean the distribution directory (and mirrors) of
                            all but the most recent MAJOR.MINOR release.  If no
                            dist-dir is given, this command will assume it is
                            running on people.apache.org.''')
    subparser.set_defaults(func=clean_dist)
    subparser.add_argument('--dist-dir',
                    help='''The directory to clean.''')

    # The move-to-dist subcommand
    subparser = subparsers.add_parser('move-to-dist',
                    help='''Move candiates and signatures from the temporary
                            post location to the permanent distribution
                            directory.  If no dist-dir is given, this command
                            will assume it is running on people.apache.org.''')
    subparser.set_defaults(func=move_to_dist)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')
    subparser.add_argument('--dist-dir',
                    help='''The directory to clean.''')
    subparser.add_argument('--code-name',
                    help='''A whimsical name for the release, used only for
                            naming the download directory.''')
    subparser.add_argument('--target',
                    help='''The full path to the destination used in
                            'post-candiates'..''')

    # The write-news subcommand
    subparser = subparsers.add_parser('write-news',
                    help='''Output to stdout template text for use in the news
                            section of the Subversion website.''')
    subparser.set_defaults(func=write_news)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')

    subparser = subparsers.add_parser('write-announcement',
                    help='''Output to stdout template text for the emailed
                            release announcement.''')
    subparser.set_defaults(func=write_announcement)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')

    subparser = subparsers.add_parser('write-downloads',
                    help='''Output to stdout template text for the download
                            table for subversion.apache.org''')
    subparser.set_defaults(func=write_downloads)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')

    # The check sigs subcommand
    subparser = subparsers.add_parser('check-sigs',
                    help='''Output to stdout the signatures collected for this
                            release''')
    subparser.set_defaults(func=check_sigs)
    subparser.add_argument('version', type=Version,
                    help='''The release label, such as '1.7.0-alpha1'.''')
    subparser.add_argument('--target',
                    help='''The full path to the destination used in
                            'post-candiates'..''')

    # A meta-target
    subparser = subparsers.add_parser('clean',
                    help='''The same as the '--clean' switch, but as a
                            separate subcommand.''')
    subparser.set_defaults(func=cleanup)

    # Parse the arguments
    args = parser.parse_args()

    # first, process any global operations
    if args.clean:
        cleanup(args)

    # Set up logging
    logger = logging.getLogger()
    if args.verbose:
        logger.setLevel(logging.DEBUG)
    else:
        logger.setLevel(logging.INFO)

    # Fix up our path so we can use our installed versions
    os.environ['PATH'] = os.path.join(get_prefix(args.base_dir), 'bin') + ':' \
                                                            + os.environ['PATH']

    # finally, run the subcommand, and give it the parsed arguments
    args.func(args)


if __name__ == '__main__':
    main()

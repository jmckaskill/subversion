#!/usr/bin/perl
use strict;
use Test::More tests => 6;
use File::Path q(rmtree);
use File::Temp qw(tempdir);

# shut up about variables that are only used once.
# these come from constants and variables used
# by the bindings but not elsewhere in perl space.
no warnings 'once';

require SVN::Core;
require SVN::Repos;
require SVN::Fs;
require SVN::Delta;

package TestEditor;
our @ISA = qw(SVN::Delta::Editor);

sub add_directory {
    my ($self, $path, undef, undef, undef, $pool) = @_;
    $pool->default;
    main::is_pool_default ($pool, 'default pool from c calls');
}

package main;
sub is_pool_default {
    my ($pool, $text) = @_;
    is (ref ($pool) eq 'SVN::Pool' ? $$$pool : $$pool,
	$$SVN::_Core::current_pool, $text);
}

my $repospath = tempdir('svn-perl-test-XXXXXX', TMPDIR => 1);

my $repos;

ok($repos = SVN::Repos::create("$repospath", undef, undef, undef, undef),
   "create repository at $repospath");

my $fs = $repos->fs;

my $pool = SVN::Pool->new_default;

is_pool_default ($pool, 'default pool');

{
    my $spool = SVN::Pool->new_default_sub;
    is_pool_default ($spool, 'lexical default pool default');
}

is_pool_default ($pool, 'lexical default pool destroyed');

my $root = $fs->revision_root (0);

my $txn = $fs->begin_txn(0);

$txn->root->make_dir('trunk');

$txn->commit;


SVN::Repos::dir_delta ($root, '', '',
		       $fs->revision_root (1), '',
		       TestEditor->new(),
		       undef, 1, 1, 0, 1);


is_pool_default ($pool, 'default pool from c calls destroyed');

END {
diag('cleanup');
rmtree($repospath);
}

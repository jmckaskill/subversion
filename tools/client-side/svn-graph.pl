#!/usr/bin/perl -w
#
# svn-graph.pl - produce a GraphViz .dot graph for the branch history
#                of a node
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
# This software consists of voluntary contributions made by many
# individuals.  For exact contribution history, see the revision
# history and logs, available at http://subversion.tigris.org/.
# ====================================================================
#
# TODO:
#   - take some command line parameters (url, start & end revs, 
#     node we're tracking, etc)
#   - calculate the repository root at runtime so the user can pass
#     the node of interest as a single URL
#   - produce the graphical output ourselves (SVG?) instead
#     of using .dot?
#

use strict;

# Turn off output buffering
$|=1;

require SVN::Core;
require SVN::Ra;

# CONFIGURE ME:  The URL of the Subversion repository we wish to graph.
# See TODO.
my $REPOS_URL = 'file:///some/repository';
#my $REPOS_URL = 'http://svn.collab.net/repos/svn';

# Point at the root of a repository so we get can look at
# every revision.  
my $ra = SVN::Ra->new($REPOS_URL);

# We're going to look at all revisions
my $youngest = $ra->get_latest_revnum();
my $startrev = 1;

# This is the node we're interested in
my $startpath = "/trunk";

# The "interesting" nodes are potential sources for copies.  This list
#   grows as we move through time.
# The "tracking" nodes are the most recent revisions of paths we're
#   following as we move through time.  If we hit a delete of a path
#   we remove it from the tracking array (i.e. we're no longer interested
#   in it).
my %interesting = ("$startpath:$startrev",1);
my %tracking = ("$startpath", $startrev);

my %codeline_changes_forward = ();
my %codeline_changes_back = ();
my %copysource = ();
my %copydest = ();

# This function is a callback which is called for every revision
# as we traverse
sub process_revision {
  my $changed_paths = shift;
  my $revision = shift || "";
  my $author = shift || "";
  my $date = shift || "";
  my $message = shift || "";
  my $pool = shift;

  print STDERR "$revision\r";

  foreach my $path (keys %$changed_paths) {
    my $copyfrom_path = $$changed_paths{$path}->copyfrom_path;
    my $copyfrom_rev = $$changed_paths{$path}->copyfrom_rev;
    my $action = $$changed_paths{$path}->action;

    # See if we're deleting one of our tracking nodes
    if ($action eq "D" and exists($tracking{$path})) 
    {
      print "\t\"$path:$tracking{$path}\" ";
      print "[label=\"$path:$tracking{$path}\\nDeleted in r$revision\",color=red];\n";
      delete($tracking{$path});
      next;
    }

    # If this is a copy, work out if it was from somewhere interesting
    if (defined($copyfrom_path) && 
        exists($interesting{$copyfrom_path.":".$copyfrom_rev})) 
    {
      $interesting{$path.":".$revision} = 1;
      $tracking{$path} = $revision;
      print "\t\"$copyfrom_path:$copyfrom_rev\" -> ";
      print " \"$path:$revision\" [label=\"copy at r$revision\",weight=1,color=green];\n";
      $copysource{"$copyfrom_path:$copyfrom_rev"} = 1;
      $copydest{"$path:$revision"} = 1;
    }

    # For each change, we'll move up the path, updating any parents
    # that we're tracking (i.e. a change to /trunk/asdf/foo updates
    # /trunk).  We mark that parent as interesting (a potential source
    # for copies), draw a link, and update it's tracking revision.
    while ($path =~ m:/:) {
      if (exists($tracking{$path}) && $tracking{$path} != $revision) {
        $codeline_changes_forward{"$path:$tracking{$path}"} =
          "$path:$revision";
        $codeline_changes_back{"$path:$revision"} =
          "$path:$tracking{$path}";
        $interesting{$path.":".$revision} = 1;
        $tracking{$path} = $revision;
      }
      $path =~ s:/[^/]+$::;
    }
  }

}

# And we can do it all with just one call to SVN :)
print "digraph tree {\n";

$ra->get_log(['/'], $startrev, $youngest, 1, 0, \&process_revision); 

# Now ensure that everything is linked.
foreach my $codeline_change (keys %codeline_changes_forward) {

  # If this node is not the first in its codeline chain, and it isn't
  # the source of a copy, it won't be the source of an edge
  if (exists($codeline_changes_back{$codeline_change}) &&
      !exists($copysource{$codeline_change})) {
    next;
  }

  # If this node is the first in it's chain, or the source of
  # a copy, then we'll print it, and then find the next in
  # the chain that needs to be printed too
  if (!exists($codeline_changes_back{$codeline_change}) or
       exists($copysource{$codeline_change}) ) {
    print "\t\"$codeline_change\" -> ";
    my $nextchange = $codeline_changes_forward{$codeline_change};
    my $changecount = 1;
    while (defined($nextchange)) {
      if (exists($copysource{$nextchange}) or
          !exists($codeline_changes_forward{$nextchange}) ) {
        print "\"$nextchange\" [weight=100,label=\"$changecount change(s)\",style=bold];";
        last;
      }
      $changecount++;
      $nextchange = $codeline_changes_forward{$nextchange};
    }
    print "\n";
  }
}

print "}\n";
print STDERR "\n";

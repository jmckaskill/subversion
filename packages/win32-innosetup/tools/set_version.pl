#!perl
################################################################################
# FILE     set_version.pl
# PURPOSE  Setting version info on misc. files for Inno Setup
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

################################################################################
# INCLUDED LIBRARY FILES
use strict;
use Cwd;
use Win32;
require 'cmn.pl';

################################################################################
# FUNCTION DECLARATIONS
sub Main;
sub PathSetupOut;
sub PathSvn;
sub SetVerSvnIss;
sub SetVersion;
sub SvnVersion;

################################################################################
# CONSTANTS AND GLOBAL VARIABLES
my $g_AutoRun='';

##########################################################################
# PROGRAM ENTRANCE
Main;

################################################################################
# FUNCTION DEFINITIONS
#-------------------------------------------------------------------------------
# FUNCTION   Main
# DOES       This is the program's main function
sub Main
{
    my $Arg=$ARGV[0];

    if ($Arg eq "-a")
      {
        $g_AutoRun="y";
      }

    my ($SvnVersion, $SvnRevision) = &SetVersion;
    my $PathSetupOut = &PathSetupOut;

    if (! $g_AutoRun)
      {
        print "Setting version $SvnVersion and revision $SvnRevision on...\n";
      }

    #Set version info on svn.iss
    &SetVerSvnIss($SvnVersion, $SvnRevision);
}

#-------------------------------------------------------------------------------
# FUNCTION PathSetupOut
# DOES     Finding and returning the current svn.exe path as of
#          ..\svn_iss_dyn.iss
sub PathSetupOut
{
    my $SetupOut = &cmn_ValuePathfile('path_setup_out');
  
    if ( ! -e "../$SetupOut")
      {
        die "ERROR: Could not find $SetupOut in ..\\svn_dynamics.iss\n";
      }
    
    return $SetupOut;
}

#-------------------------------------------------------------------------------
# FUNCTION PathSvn
# DOES     Finding and returning the current svn.exe path as of
#          ..\svn_dynamics.iss
sub PathSvn
{
    my $RetVal = &cmn_ValuePathfile('path_svnclient');
    my $ErrMsg="ERROR: File not found: Could not find svn.exe in:\n  $RetVal\n";
    $ErrMsg=$ErrMsg . "Please, check that the path_svnclient variable in the ";
    $ErrMsg=$ErrMsg . "..\\svn_dynamics.iss\n";
    $ErrMsg=$ErrMsg . "file are correct and try again\n";
    
    if (-e "$RetVal\\svn.exe")
      {
        $RetVal="$RetVal\\svn.exe";
      }
    else
      {
        die $ErrMsg;
      }

    return $RetVal;
}

#-------------------------------------------------------------------------------
# FUNCTION SetVersion
# DOES     Gets and returns version info from userinput
sub SetVersion
{
    my ($SvnVersion, $SvnRevision) = &SvnVersion;
    my ($InputVersion, $InputRevision)='';

    $SvnRevision = "unset" if (! $SvnRevision);
    
    if (! $g_AutoRun)
      {
        print "\nsvn.exe that's mentioned in your svn_dynamics.iss file have ",
          "told me that the\n",
          "version you want to make a distro from is $SvnVersion and that the ",
          "revision is\n",
          "$SvnRevision. You can confirm this by hitting the ENTER button ",
          "(wich then sets the numbers\n",
          "inside the brackets) or write some new data followed by the ENTER",
          " button.\n\n",
          "Please, make sure that svn.iss is not opened by another ",
          "applications before you continue:\n\n";
          
          print "  Version [$SvnVersion]: ";
        
        chomp ($InputVersion = <STDIN>);

        if ($InputVersion)
          {
            $SvnVersion = $InputVersion;
            
          }

        $SvnRevision = "" if ($SvnRevision eq "unset");
        print "  Revision [$SvnRevision]: ";
        chomp ($InputRevision = <STDIN>);

        if ($InputRevision)
          {
            $SvnRevision = $InputRevision;
          }
      }

    return ($SvnVersion, $SvnRevision);
}

#-------------------------------------------------------------------------------
# FUNCTION SetVerSvnIss
# DOES     Setting version info on svn.iss
sub SetVerSvnIss
{
    my ($SvnVersion, $SvnRevision) = @_;
    my $SvnPreTxtRevision='';
    my $IssFileCnt='';

    $SvnPreTxtRevision='-r' if ($SvnRevision);

    if (! -e '../svn_version.iss')
      {
        system ("copy ..\\templates\\svn_version.iss ..");
      }

    print "  svn_version.iss in the Inno Setup directory.\n" if (! $g_AutoRun);

    open (FH_ISSFILE, '../svn_version.iss') || die "ERROR: Could not open ..\\svn_version.iss";
    while (<FH_ISSFILE>)
      {
			  chomp($_);

        if ($IssFileCnt)
          {
            $IssFileCnt="$IssFileCnt\n";
          }

        if (/^#define svn_version/)
          {
              $IssFileCnt= $IssFileCnt . "#define svn_version \"$SvnVersion\"";
          }
        elsif (/^#define svn_revision/)
          {
              $IssFileCnt= $IssFileCnt . "#define svn_revision \"$SvnRevision\"";
          }
        elsif (/^#define svn_pretxtrevision/)
          {
              $IssFileCnt= $IssFileCnt . "#define svn_pretxtrevision \"$SvnPreTxtRevision\"";
          }
        else
          {
              $IssFileCnt= $IssFileCnt . $_;
          }
      }
    close (FH_ISSFILE);  

    $IssFileCnt="$IssFileCnt\n";

    open (FH_ISSFILE, ">" . '../svn_version.iss')
      || die "ERROR: Could not open ..\\svn_version.iss";
		    print FH_ISSFILE $IssFileCnt;
	  close (FH_ISSFILE);
}

#-------------------------------------------------------------------------------
# FUNCTION SvnVersion
# DOES     Getting and returns the version and revision number from the svn.exe
#          as of the binary to include in the distro
sub SvnVersion
{
    my $Svn = &PathSvn;
    my @SvnVerOut;
    my $SvnRetVal='';
    my ($SvnVersion, $SvnRevision) ='';

    $Svn = "\"$Svn\"";
    $SvnRetVal = `$Svn --version`;

    @SvnVerOut = split(/\n/, $SvnRetVal);

    for (@SvnVerOut)
      {
        if (/svn, version /)
          {
            $SvnRetVal = $_;
            last;          
          }
      }

    $SvnRetVal =~ s/svn, version //s;

    if ($SvnRetVal =~ /.+\(r.+\)/)
      {
        $SvnRetVal =~ s/(^.*)\).*/$1/;
        ($SvnVersion, $SvnRevision) = split (/\(/, $1);
      }
    else
      {
        $SvnRetVal =~ s/([\d]?\.[\d]{1,2}\.[\d]{1,2})/$1/;
        $SvnVersion = $SvnRetVal;
      }

    $SvnVersion =~ s/^\s+//;
	   $SvnVersion =~ s/\s+$//;
    $SvnRevision =~ s/r//;
    $SvnRevision =~ s/dev build/_dev-build/;

    return ($SvnVersion, $SvnRevision);
}

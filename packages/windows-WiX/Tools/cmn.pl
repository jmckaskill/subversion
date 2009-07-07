# ==============================================================================
# Common Perl routines for the Perl helper scripts.
# ==============================================================================
# Copyright (c) 2000-2009 CollabNet.  All rights reserved.
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
# ==============================================================================

##########################################################################
# INCLUDED LIBRARY FILES
use File::Basename;
use XML::XPath;

#-------------------------------------------------------------------------------
# FUNCTION   cmn_IniDir
# DOES       Returns the directory where the initialization file is. The
#            dir is application directory of the current user 
sub cmn_IniDir
{
    my $DirAppData='';
  
    # The registry is the safe way of retrieving the Application data directory,
    # but we let the environment variable %APPDATA% have the priority. This 
    # should work on every Win32 platform.
    if ($ENV{'APPDATA'})
      {
        $DirAppData = $ENV{'APPDATA'};
      }
    else
      {
        my $Key = 'HKCU/Software/Microsoft/Windows/CurrentVersion/Explorer/Shell Folders';
        my $Value = 'AppData';
        $DirAppData = &cmn_RegGetValue ($Key, $Value);
      }

    return "$DirAppData\\Subversion";
}

#-------------------------------------------------------------------------------
# FUNCTION   cmn_RegGetValue
# RECEIVES   The Key and value.
# DOES       Returns a value data  from the registry. If the value is
#            omitted then the default value data for the key is returned
sub cmn_RegGetValue
{
    use Win32::TieRegistry;
  
    my ($Key, $Value) = @_;
  
    # Replace back slashes with slashes
    $Key =~ s/\\/\//g;
  
    # Do some filtering if the caller includes HKLM in stead of HKEY_LOCAL_MACHINE
    # or the Win32::TieRegistry shortcut LMachine and so on
    $Key =~ s/^HKCC/CConfig/;
    $Key =~ s/^HKCR/Classes/;
    $Key =~ s/^HKCU/CUser/;
    $Key =~ s/^HKDD/DynData/;
    $Key =~ s/^HKLM/LMachine/;
    $Key =~ s/^HKPD/PerfData/;
    $Key =~ s/^HKUS/Users/;
  
    $Registry->Delimiter("/");

    return $Registry -> {"$Key//$Value"};
}

#-------------------------------------------------------------------------------
# FUNCTION   incCmn_Template
# RECEIVES   An hash table with values, template name
# RETURNS    The contents of the template with the received values
# DOES       Reading from a template and fills in values in <%..%> tags if any
sub cmn_Template
{
 	  my ($sFile, $sValues) = @_;
 	  my $sFileCnt='';

 	  local $/;
 	  local *FH_TPL;

 	  open (FH_TPL, "< $sFile\0")	|| return;
 	      $sFileCnt = <FH_TPL>;
 	  close (FH_TPL);

 	  $sFileCnt =~ s{<% (.*?) %>}

 	  {exists ( $sValues->{$1}) ? $sValues->{$1} : '' }gsex;

 	  return $sFileCnt;
}

#-------------------------------------------------------------------------------
# FUNCTION cmn_ValuePathfile
# DOES     Get and returns a ISPP variable value from a file
sub cmn_ValuePathfile
{
    my $IssFile = $_[0];
    my $VarISPP = $_[1];
    my $RetVal='';
    my $ErrNoPathFile='';
#    my $IssFile = "..\\svn_dynamics.iss";

    $ErrNoPathFile="ERROR: $IssFile not found. please, make sure that it's ";
    $ErrNoPathFile=$ErrNoPathFile . "where it\n should to be\n";

    $VarISPP = "#define " . $VarISPP;

    open (FH_ISSFILE, $IssFile) || die $ErrNoPathFile;
    while (<FH_ISSFILE>)
      {
			  chomp($_);

        if (/^$VarISPP/)
          {
              $_ =~ s/^$VarISPP//;
              $_ =~ s/^\s+//;
              $_ =~ s/\s+$//;
              $_ =~ s/^"//;
              $_ =~ s/"$//;
              $RetVal = $_;
              last;
          }
      }
    close (FH_ISSFILE);

    return $RetVal;
}

#FUNCTION   UpdateXML
#DOES            Change XML content and attributes
sub UpdateXMLFile
{
	#my $XMLFile = '..\\BuildSubversion\\BuildSubversion.wixproj';
	my $XMLFile = $_[0];
	my $XMLPath = $_[1];
	my $XMLNewValue = $_[2];
	my $PINode = $_[3];
	#my $XMLType = $_[3];
	
	my $xp = XML::XPath->new(filename=>$XMLFile);
	
        if(!$PINode) {
		$xp->setNodeText($XMLPath, $XMLNewValue);

		#output node by node to rebuild the file
		my $nodeset = $xp->find('/');

		open(MYFILE, '>'.$XMLFile);

		foreach my $node ($nodeset->get_nodelist) {
			print MYFILE XML::XPath::XMLParser::as_string( $node );
		}

		close(MYFILE);
	} else {
		my $XMLOut = '';
		my $OrigPI = $xp->findvalue( $XMLPath );

		#build the file into a string
		my $nodeset = $xp->find('/');

		foreach my $node ($nodeset->get_nodelist) {
			$XMLOut = $XMLOut.XML::XPath::XMLParser::as_string( $node );
		}
		
		$XMLOut =~ s/$OrigPI/$XMLNewValue /;
		
		open(MYFILE, '>'.$XMLFile);
		print MYFILE "<?xml version=\"1.0\"?>\n"; #this is hard coded - ASSUMING that PI's are being handled for wxs files
		print MYFILE $XMLOut;
		close(MYFILE);
	}
}

#-------------------------------------------------------------------------
# FUNCTION   MkDirP
# DOES       Making a directory. Similar to unix's mkdir -p
sub MkDirP
{
    my $Dir=$_[0];
    my @SubPaths;

    
    
    if (! -e $Dir)
      {
        @SubPaths = split (/\\/, $Dir);
        my $Dir2Make='';
        for (@SubPaths)
          {
            if ($Dir2Make)
              {
                $Dir2Make = "$Dir2Make\\$_";
              }
            else
              {
                $Dir2Make = $_;
              }

            if (! -e $Dir2Make)
              {
                system ("mkdir $Dir2Make");
              }
          }
      }
}

#-------------------------------------------------------------------------------
# FUNCTION PathSetupOut
# DOES     Finding and returning the current svn.exe path as of
#          ..\svn_iss_dyn.iss
sub PathSetupOut
{
    my $PathWinIsPack='';
    my $Pwd='';

    # Get absolute path of the current PWD's parent
    $PathWinIsPack=getcwd;
    $Pwd=basename($PathWinIsPack);
    $PathWinIsPack =~ s/\//\\/g;
    $PathWinIsPack =~ s/\\$Pwd$//;

    my $SetupOut = "$PathWinIsPack\\" . &cmn_ValuePathfile('svn_dynamics.ini', 'path_setup_out');

    #Make the out dir in "$RootSvnBook\src if needed
    &MkDirP ("$SetupOut") unless (-e "$SetupOut");

#    if ( ! -e "../$SetupOut")
    if ( ! -e "$SetupOut")
      {
        die "ERROR: Could not find $SetupOut in svn_dynamics.ini\n";
      }
    
    return $SetupOut;
}

1;

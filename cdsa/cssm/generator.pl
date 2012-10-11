#!/usr/bin/perl
#
# generator.pl - derive various and sundry C++ code from the CDSA header files
#
# Usage:
#	perl generator.pl input-directory output-directory
#
# Perry The Cynic, Fall 1999.
#
@API_H=("cssmapi.h");
%SPI_H=("AC" => "cssmaci.h", "CSP" => "cssmcspi.h", "DL" => "cssmdli.h",
        "CL" => "cssmcli.h", "TP"  => "cssmtpi.h");
@OIDS_H=("oidscert.h", "oidscrl.h", "oidsattr.h", "oidsalg.h");

$SOURCEDIR=$ARGV[0];		# directory with inputs

(${D}) = $SOURCEDIR =~ m@([/:])@;		# guess directory delimiter
sub macintosh() { return ${D} eq ':'; }

# XXX The configuration file should be passed in as a command line argument
if( macintosh() ){
  $TARGETDIR=$ARGV[2];					# directory for outputs
  $APICFG=":::cdsa:cdsa:generator.cfg";				# configuration file
}
else{
  $TARGETDIR=$ARGV[1];					# directory for outputs
  $APICFG="generator.cfg";		# configuration file 
}


$TRANSITION="$TARGETDIR${D}transition.gen"; # C++ code for transition layer
$TABLES="$TARGETDIR${D}funcnames.gen";		# function name tables
$REPORT="$TARGETDIR${D}generator.rpt";		# report file
$EXPORTS="$TARGETDIR${D}cssmexports.gen";	# Exports file

$tabs = "\t\t\t";	# argument indentation (noncritical)
$warning = "This file was automatically generated. Do not edit on penalty of futility!";


#
# Parse API headers and extract function names and argument lists
#
$/=undef;	# big gulp mode
foreach $_ (@API_H) {
  open(API_H, "$SOURCEDIR${D}$_") or die "Cannot open $SOURCEDIR${D}$_: $^E";
  $_ = <API_H>;		# glglgl... aaaaah
  tr/\012/\015/ if macintosh;
  %formals = /CSSM_RETURN CSSMAPI\s*([A-Za-z_]+)\s+\(([^)]*)\);/gs;
  while (($name, $args) = each %formals) {
    $args =~ s/^.*[ *]([A-Za-z_]+,?)$/$tabs$1/gm;	# remove type declarators
    $args =~ s/^$tabs//o;	# chop intial tabs		# so now we have...
    $actuals{$name} = $args;						# ...an actual argument list
  };
};
close(API_H);


#
# Slurp SPI headers into memory for future use
#
$/=undef;	# slurp files
while (($key, $file) = each %SPI_H) {
  open(SPI_H, "$SOURCEDIR${D}$file") or die "Cannot open $SOURCEDIR${D}$file: $^E";
  $spi{$key} = <SPI_H>;
  $spi{$key} =~ tr/\012/\015/ if macintosh;
};
close(SPI_H);


#
# Open and read the configuration file
#
$/=undef;	# gulp yet again
open(APICFG, $APICFG) or die "Cannot open $APICFG: $^E";
$_=<APICFG>;
close(APICFG);
tr/\012/\015/ if macintosh;
%config = /^\s*(\w+)\s+(.*)$/gm;


#
# Now we will generate the API transition layer.
# The idea here is that for each function in the API, we try to
# figure out what type of plugin it belongs to, and then look up
# its evil twin in that type's header. If that works, we generate
# a function for the transition, taking care of various oddities
# and endities in the process.
#
open(OUT, ">$TRANSITION") or die "Cannot write $TRANSITION: $^E";
select OUT;
open(REPORT, ">$REPORT") or die "Cannot write $REPORT: $^E";

sub ignored {
  my ($reason) = @_;
  $ignored++;
  print REPORT "$name $reason\n";
};

print "//
// $warning
//";

for $name (sort keys %formals) {
  $config = $config{$name};
  do { ignored "has custom implementation"; next; } if $config =~ /custom/;

  ($barename) = $name =~ /^CSSM.*_([A-Za-z]+$)/;
  die "Can't fathom SPI name for $name" unless $barename;
  $actuals = $actuals{$name};

  # key off the type code in the first argument: CSSM_type_HANDLE
  ($type, $handle) = $formals{$name} =~ /CSSM_([A-Z_]+)_HANDLE\s+([A-Za-z0-9_]+)/;
  $type = "CSP" if $type eq "CC";	# CSP methods may have CC (context) handles
  $type = "DL" if $type eq "DL_DB";	# DL methods may have DL_DB handles
  $type = "KR" if $type eq "KRSP";	# KR methods have KRSP handles
  $Type = $type . "Attachment";
  do { ignored "has no module type"; next; } unless defined $SPI_H{$type};

  # $prefix will hold code to be generated before the actual call
  $prefix = "";

  # match the SPI; take care of the Privilege variants of some calls
  ($args) = $spi{$type} =~ /CSSM_RETURN \(CSSM${type}I \*$barename\)\s+\(([^)]*)\);/s
    or $barename =~ s/P$// &&	# second chance for FooBarP() API functions
      (($args) = $spi{$type} =~ /CSSM_RETURN \(CSSM${type}I \*$barename\)\s+\(([^)]*)\);/s)
    or do { ignored "not in $SPI_H{$type}"; next; };

  # take care of CSP calls taking context handles
  $handletype = $type;
  $type eq "CSP" && $actuals =~ /CCHandle/ && do {
    $actuals =~ s/CCHandle/context.CSPHandle, CCHandle/;
    $args =~ /CSSM_CONTEXT/ &&
      $actuals =~ s/CCHandle/CCHandle, &context/;
    $handletype = "CC";
  };

  # add the default privilege argument to non-P functions taking privileges
  $args =~ /CSSM_PRIVILEGE/ && ! ($name =~ /P$/) &&			# add privilege argument (last)
    ($actuals .= ",\n${tabs}attachment.module.cssm.getPrivilege()");

  # finally translate DLDBHandles into their DL component
  $handle =~ s/DLDBHandle/DLDBHandle.DLHandle/;

  # payoff time
  print "
CSSM_RETURN CSSMAPI
$name ($formals{$name})
{
  BEGIN_API";
  if ($handletype eq "CC") {
    print "
  HandleContext &context = enterContext($handle);
  CSPAttachment &attachment = context.attachment;";
  } else {
    print "
  $Type &attachment = enterAttachment<$Type>($handle);";
  };
  print "
  TransitLock _(attachment);
  ${prefix}return attachment.downcalls.$barename($actuals);
  END_API($type)
}
";
  $written++;
};
close(OUT);
select(STDOUT);


#
# Now peruse the SPI headers for a list of function names
# and build in-memory translation tables for runtime.
#
open(OUT, ">$TABLES") or die "Cannot write $TABLES: $^E";
select OUT;

print "//
// Standard plugin name tables
// $warning
//
";
while (($name, $_) = each %spi) {
  print "extern const char *const ${name}NameTable[] = {";
  s/^.*struct cssm_spi.*{(.*)} CSSM_SPI.*$/$1/s
    or die "bad format in $SPI_H{$name}";
  s/CSSM_RETURN \(CSSM[A-Z]*I \*([A-Za-z_]+)\)\s+\([^)]+\);/\t"$1",/g;
  print;
  print "};\n\n";
};
close(OUT);
select(STDOUT);

#
# Finally, generate linker export file to avoid leaking internal symbols
#
open(OUT, ">$EXPORTS") or die "Cannot write $EXPORTS: $^E";
select(OUT);

# entry point names (functions)
for $name (keys %formals) {
  $symbols{$name} = 1;
};

# OID-related data symbols
$/=undef;
foreach $_ (@OIDS_H) {
  open(OIDS_H, "$SOURCEDIR${D}$_") or die "Cannot open $SOURCEDIR${D}$_: $^E";
  $_ = <OIDS_H>;		# glglgl... aaaaah
  tr/\012/\015/ if macintosh;
  s/\/\*.*\*\///gm;	# remove comments
  
  foreach $name (/\s+(CSSMOID_[A-Za-z0-9_]+)/gs) {
    $symbols{$name} = 1;
  };
};
close(OIDS_H);

foreach $name (keys %symbols) {
  print "_$name\n";
};

close(OUT);
select(STDOUT);


close(EXPORTS);
close(REPORT);
print "$written API functions generated; $ignored ignored (see $REPORT)\n";

#!/usr/bin/perl
use strict;
use warnings;
use File::Basename;
use Switch;
use threads;

my ($output, $first_app_arg);

sub HELP_MESSAGE() {
   print "Usage:\tminiprof-exec [-o miniprof-output-file] [-<miniprof option> <miniprof arg>] ./app <args>\n";
   print "\tSupported miniprof options:\n";
   print "\t\t-e NAME COUNTER KERNEL USER PER_DIE\n";
   print "\t\t-c NB_CORES\n";
   print "\t\t-t TID\n";
   print "\t\t-a APPLICATION\n";
   print "\t\t-ft\n";
   exit;
}

#Get -o if exists
HELP_MESSAGE if($#ARGV < 0);
if($ARGV[0] eq '-o') {
   shift;
   $output = shift;
}

#Get miniprof options
for (my $index = 0; !defined $first_app_arg;) {
   HELP_MESSAGE if($index > $#ARGV);
   my $val = $ARGV[$index];
   switch ($val) {
      case "-e" { $index += 6; }
      case "-c" { $index += 2; }
      case "-t" { $index += 2; }
      case "-a" { $index += 2; }
      case "-ft" { $index += 1; }
      else { $first_app_arg = $index; }
   }
}
HELP_MESSAGE if($first_app_arg > $#ARGV);


my $miniprof_thr = async {
   my $dirname = dirname(__FILE__);
   my $mini_cmd = "sudo $dirname/miniprof ";
   $mini_cmd .= join(' ', @ARGV[0..($first_app_arg-1)]) if($first_app_arg > 0);
   $mini_cmd .= " &> $output" if(defined $output);
   system($mini_cmd);
};

my $cmd = join(" ", @ARGV[$first_app_arg..$#ARGV]);
system($cmd);

print `sudo killall miniprof`;
$miniprof_thr->join();

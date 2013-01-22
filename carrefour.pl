#!/usr/bin/perl
use warnings;
use strict;
use threads;

use Time::HiRes;
use Expect;
use Data::Dumper;
use FindBin qw($Bin);

#my @profiling_times = ( 0.5 );
my @profiling_times = ( 1 );
#my @profiling_times = ( 1.5 );
#my @profiling_times = ( 2 );
#my @profiling_times = ( 3 );
#my @profiling_times = ( 5 );

sub launch {
   my ($prof_time) = @_;
   my $total_time;

   my $cmd = "/usr/bin/time";
   my @args = ( "-p" );
   for my $a (@ARGV) {
      push @args, $a;
   }
   
   print "Running $cmd ".(join " ", @args)."\n";

   my $exp = new Expect;
   $exp->raw_pty(1);
   $exp->log_stdout(1);
   $exp->spawn($cmd, @args) or die "Cannot spawn $cmd: $!\n";

   do {
      system "echo 'b' > /proc/inter_cntl";   
      Time::HiRes::sleep($prof_time);
      system "echo 'e' > /proc/inter_cntl";
      $exp->expect(0, [ qr/real (\d+\.\d+)/ , sub {$total_time = ($exp->matchlist)[0];} ]);
   }
   while(!$exp->error());

   printf "\nGlobal time = %.1f s\n", $total_time;
}

print  "sudo rmmod carrefour\n";
system "sudo rmmod carrefour";
print  "sudo insmod $Bin/carrefour.ko\n";
system "sudo insmod $Bin/carrefour.ko";

for my $t (@profiling_times) {
   launch($t);
}

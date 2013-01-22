#!/usr/bin/perl
use warnings;
use strict;
use File::stat;

die('Usage: ./create_hooks.pl directory_of_carrefour_module') if(!$ARGV[0]);


#The symbols we want to find and the file where to put them
my $hook_file_name = $ARGV[0].'/carrefour_hooks.h';
my %fun_to_look_for = (
   "lru_add_drain_all" => 'int (*)(void)', 
   "find_task_by_vpid" => "struct task_struct* (*)(pid_t)",
   "tasklist_lock" => "rwlock_t* (*)",
   "setup_APIC_eilvt" => "int (*)(u8, u8, u8, u8)",
   "isolate_lru_page" => "int (*)(struct page *)",
   "new_page_node" => "struct page* (*)(struct page *, unsigned long, int **)",
   "putback_lru_pages" => "void (*)(struct list_head *l)",
   "migrate_pages" => "int (*)(struct list_head *, new_page_t, unsigned long, bool, bool sync)",
   "find_task_by_vpid" => "struct task_struct* (*)(pid_t)",
   "follow_page" => "struct page * (*)(struct vm_area_struct *, unsigned long, unsigned int)",
   #"__migrate_task" => "int (*)(struct task_struct *p, int src_cpu, int dest_cpu)",
   #"set_task_cpu" => "void (*)(struct task_struct *p, unsigned int new_cpu)",
   "sched_setaffinity" => "long (*)(pid_t pid, const struct cpumask *in_mask)",
   "ptep_clear_flush" => "pte_t (*)(struct vm_area_struct *, unsigned long address, pte_t *)"
);



#Try to find symbols in one of those files
#Exit if we have the most recent version of the symbols
my $kernel = `uname -r`; chomp($kernel);
my @symbol_files = ("/boot/System.map-$kernel" , "/boot/System.map-genkernel-x86_64-$kernel");

my $modif_time_hooks = 0;
if(-e $hook_file_name) {
   $modif_time_hooks = stat($hook_file_name)->mtime;
}
#Ignore hook file if the .pl has been updated
my $my_modif_time = stat($0)->mtime;
if($my_modif_time > $modif_time_hooks) {
   $modif_time_hooks = undef;
}

my $symbols;
for my $s (@symbol_files) {
   if(-e $s) {
      #if($modif_time_hooks && (stat($s)->mtime < $modif_time_hooks)) {
      #   exit; #Already have latest symbols
      #}
      $symbols = `cat $s`;
   }
}
die("Unable to find kernel symbols. Looked in".join(',', @symbol_files)."\n") if(!$symbols);
my @lines = split(/\n/, $symbols);


#Write symbols in the file
open(F, "> $hook_file_name") or die("Cannot open $hook_file_name for writting\n");
print F "#ifndef SDP_HOOKS\n";
print F "#define SDP_HOOKS\n\n";

for my $l (@lines) {
   next if($l !~ m/([a-f0-9]+)\s(\w)\s(.*)/);
   my $h = $1;
   my $f = $3;
   next if($2 ne 'T' && $f eq 'putback_lru_pages');
   if($fun_to_look_for{$f}) {
      my $type = $fun_to_look_for{$f};
      my $fun_hook = $type;
      $fun_hook =~ s/(\(\*\))/(*${f}_hook)/;
      print F "static __attribute__((unused)) $fun_hook = ($type) 0x$h;\n";
      #$fun_to_look_for{$f} = undef;
   }
}

print F "\n#endif\n";


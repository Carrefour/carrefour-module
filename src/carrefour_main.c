/*
Copyright (C) 2013  
Fabien Gaud <fgaud@sfu.ca>, Baptiste Lepers <baptiste.lepers@inria.fr>,
Mohammad Dashti <mdashti@sfu.ca>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "carrefour_main.h"

/**
 * /proc/inter_cntl
 */
static int running;
extern int enable_carrefour;

extern unsigned enable_replication;
extern unsigned enable_interleaving;
extern unsigned enable_migration;
extern unsigned enable_splitting;
extern unsigned enable_hotpage_tracking;

extern unsigned long global_maptu;
extern unsigned long sampling_rate;
extern unsigned long nr_accesses_node[MAX_NUMNODES];

static enum thp_states thp_initial_state;

static ssize_t ibs_proc_write(struct file *file, const char __user *buf,
      size_t count, loff_t *ppos) {
   char c;

   if (count) {
      if (get_user(c, buf))
         return -EFAULT;
      if (c == 'b' && !running) {
         start_profiling();
         running = 1;
      } 
      else if (c == 'e' && running) {
         enable_carrefour = 1;
         stop_profiling();
         running = 0;
      } 
      else if (c == 'x' && running) {
         enable_carrefour = 0;
         stop_profiling();
        
         if(carrefour_module_options[ADAPTIVE_SAMPLING].value) { 
            //printk("[ADAPTIVE] Carrefour disabled, reducing the IBS sampling rate\n");
            sampling_rate = (unsigned long) carrefour_module_options[IBS_RATE_CHEAP].value;
         }

         start_profiling();
      }
      else if (c == 'k' && running) {
         enable_carrefour = 0;
         stop_profiling();
         running = 0;
      }
      else if (c == 'i') {
         enable_interleaving = 0;
      }
      else if (c == 'I') {
         enable_interleaving = 1;
      }
      else if (c == 'T') {
         if(count > 1) {
            /* get buffer size */
            char * buf_tmp = kmalloc(count, GFP_KERNEL | __GFP_ZERO);
            char * index = buf_tmp;
            char * next_idx;
            int node = 0;

            if (copy_from_user(buf_tmp, buf, count)) {
               return -EFAULT;
            }
           
            // Skip the I
            index++;

            for (next_idx = index; next_idx < buf_tmp + count; next_idx++) {
               if(*next_idx == ',' || next_idx == (buf_tmp + count -1)) {
                  unsigned long value;
                  if(*next_idx == ',') {
                     *next_idx = 0;
                  }

                  if(kstrtol(index, 10, &value) < 0) {
                     printk("Value is %s (%lu)\n", index, value); 
                     printk(KERN_WARNING "Strange bug\n");
                     memset(nr_accesses_node, 0, sizeof(unsigned long) * MAX_NUMNODES);
                     break;
                  }
                  nr_accesses_node[node++] = value;
                  index = next_idx+1;

                  //printk("Node %d --> %lu\n", node -1, nr_accesses_node[node-1]);
               }
            }
            
            kfree(buf_tmp); 
         }
      }
      else if (c == 'Z') {
         int pid, enabled;
         int ret = sscanf(buf, "Z\t%d\t%d\n", &pid, &enabled);
         if(ret != 2) {
            printk("Error %s\n", buf);
         } else {
            printk("Replication for pid %d => %d\n", pid, enabled);
            change_replication_state(pid, enabled);

            if(enabled) {
               enable_replication = 1;
            }
         }
      }
      else if (c == 'A') {
         int ret = sscanf(buf, "A%lu\n", &global_maptu);
         if(ret != 1) {
            printk("Error %s\n", buf);
         } 
      }
      else if (c == 'r') {
         enable_replication = 0;
      }
      else if (c == 'R') {
         enable_replication = 1;
      }
      else if (c == 'M') {
         enable_migration = 1;
      }
      else if (c == 'm') {
         enable_migration = 0;
      }
      else if (c == 'S') {
         enable_splitting = 1;
      }
      else if (c == 's') {
         enable_splitting = 0;
      }
      else if (c == 'F' && carrefour_module_options[ADAPTIVE_SAMPLING].value) {
         // Increases the ibs frequency
         sampling_rate = carrefour_module_options[IBS_RATE_ACCURATE].value;
      }
      else if (c == 'f' && carrefour_module_options[ADAPTIVE_SAMPLING].value) {
         // Decreases the ibs frequency
         sampling_rate = carrefour_module_options[IBS_RATE_CHEAP].value;
      }
   }
   return count;
}

extern unsigned split_has_been_chosen;
extern unsigned nr_thp_split;

unsigned carrefour_ran = 0;

static int ibs_proc_display(struct seq_file *m, void* v) {
   seq_printf(m, "%u %u %d %d\n", split_has_been_chosen, carrefour_ran, nr_thp_split, get_thp_state());

   return 0;
}

static int ibs_proc_open(struct inode *inode, struct file *file) {
   return single_open(file, ibs_proc_display, NULL);
}

static const struct file_operations ibs_cntrl_fops = { 
   .write  = ibs_proc_write,
   .open   = ibs_proc_open,
   .read    = seq_read,
   .llseek  = seq_lseek,
   .release = seq_release,
};

void ibs_create_procs_files(void) {
   proc_create("inter_cntl", S_IWUGO, NULL, &ibs_cntrl_fops);
}

void ibs_remove_proc_files(void) {
   remove_proc_entry("inter_cntl", NULL);
}

/** What to do when starting profiling **/
/** Must be called with NMI disabled   **/
u64 time_start_profiling;

int start_profiling(void) {
   rdtscll(time_start_profiling);

   rbtree_init();
   carrefour_init();

   ibs_start();

   return 0;
} 

/** And when stoping profiling         **/
/** Must be called with NMI disabled   **/
unsigned long iteration_length;

int stop_profiling(void) {
   u64 time_before_stop_profiling, time_after_stop_profiling;
   enum thp_states thp_current_state = get_thp_state();
   struct carrefour_hook_stats_t hook_stats;

   rdtscll(time_before_stop_profiling);

   iteration_length = (time_before_stop_profiling - time_start_profiling); 

   //rbtree_print(); //debug
   //show_tid_sharing_map(); //debug

   enable_migration = carrefour_module_options[ENABLE_MIGRATION].value && enable_migration;
   enable_replication = carrefour_module_options[ENABLE_REPLICATION].value && enable_replication;
   enable_interleaving = carrefour_module_options[ENABLE_INTERLEAVING].value && enable_interleaving;

   enable_splitting = carrefour_module_options[SPLIT_SHARED_THP].value && enable_splitting;
   enable_hotpage_tracking = carrefour_module_options[ENABLE_HOT_PAGE_TRACKING].value;

   if(!enable_replication && !enable_interleaving && !enable_migration && !enable_splitting) {
      enable_carrefour = 0;
   }

   printu("-- Carrefour %s, replication %s, interleaving %s, migration %s, frequency %s [THP = %s]\n", 
            enable_carrefour    ? "enabled" : "disabled",
            enable_replication  ? "enabled" : "disabled",
            enable_interleaving ? "enabled" : "disabled",
            enable_migration    ? "enabled" : "disabled",
            sampling_rate == carrefour_module_options[IBS_RATE_ACCURATE].value ? "accurate" : "cheap",
            (thp_current_state == THP_ALWAYS) ? "always" : ((thp_current_state == THP_MADVISE) ? "madvise" : "disabled")
   );

   ibs_stop();

   if(carrefour_module_options[PROFILER_MODE].value) {
      enable_carrefour = 1;
   }

   hook_stats = get_carrefour_hook_stats();
   run_stats.time_spent_in_split = hook_stats.time_spent_in_split;
   if(hook_stats.nr_4k_migrations + hook_stats.nr_2M_migrations) {
      struct carrefour_options_t options = get_carrefour_hooks_conf();

      run_stats.time_spent_in_migration = hook_stats.time_spent_in_migration_4k + hook_stats.time_spent_in_migration_2M;
      //run_stats.time_spent_in_migration = hook_stats.time_spent_in_migration_2M;

      if(options.async_4k_migrations) {
         // Because it's asynchronous, t can be done in parallel on multiple core
         // We might under-estimate it
         run_stats.time_spent_in_migration /= num_online_cpus();
      }
   }
   else {
      run_stats.time_spent_in_migration = 0;
   }

   reset_carrefour_stats();

   //Always run carrouf to split pages
   if(enable_carrefour) {
      carrefour();
      carrefour_ran = 1;
   }
   else {
      split_has_been_chosen = 0;
      carrefour_ran = 0;
   }
   
   if(carrefour_module_options[ADAPTIVE_SAMPLING].value) {
      if(!split_has_been_chosen && (run_stats.total_nr_orders < carrefour_module_options[IBS_ADAPTIVE_MAGIC].value)) {
         printk("[ADAPTIVE] Did not take enough decision (%lu). Reducing the IBS sampling rate\n", run_stats.total_nr_orders);
         sampling_rate = carrefour_module_options[IBS_RATE_CHEAP].value;
      }
      else {
         printk("[ADAPTIVE] Took lots of decisions (%lu). Increasing the IBS sampling rate\n", run_stats.total_nr_orders);
         sampling_rate = carrefour_module_options[IBS_RATE_ACCURATE].value;
      }
   }

   /** free all memory **/
   rbtree_clean();
   carrefour_clean();

   rdtscll(time_after_stop_profiling);
   if(num_online_cpus() > 0 && (time_after_stop_profiling - time_start_profiling > 0)) {
      unsigned long total_time = time_after_stop_profiling - time_start_profiling;
      unsigned long stop_profiling_time = time_after_stop_profiling - time_before_stop_profiling;

      printu("-- Carrefour %lu total profiling time, %lu stop_profiling, %lu in NMI - Overhead master core %d%%, average overhead %d%%\n",
            (unsigned long) total_time,
            (unsigned long) stop_profiling_time,
            (unsigned long) run_stats.time_spent_in_NMI,
            (int)(stop_profiling_time * 100 / total_time),
            (int)((run_stats.time_spent_in_NMI / num_online_cpus()) * 100 / total_time)
            );

      printu("-- Carrefour %lu total migration time, migration overhead (%d%% - %d%% of master job),  %lu total split time, split overhead (%d%% - %d%% of master job)\n",
            (unsigned long) run_stats.time_spent_in_migration,
            (int) (run_stats.time_spent_in_migration * 100 / (time_after_stop_profiling - time_start_profiling)),
            (stop_profiling_time)?((int) (run_stats.time_spent_in_migration * 100 / (stop_profiling_time))):0,
            (unsigned long) run_stats.time_spent_in_split,
            (int) (run_stats.time_spent_in_split * 100 / (time_after_stop_profiling - time_start_profiling)),
            (stop_profiling_time)?((int) (run_stats.time_spent_in_split * 100 / (stop_profiling_time))):0
            );
   }

   printu("Current core is %d\n", smp_processor_id());

   return 0;
}

/**
 * Init and exit
 */
extern unsigned long min_lin_address;
extern unsigned long max_lin_address;
module_param(min_lin_address, ulong, S_IRUGO);
module_param(max_lin_address, ulong, S_IRUGO);

extern struct carrefour_global_stats global_stats;

static int __init carrefour_init_module(void) {
   int err;
   struct carrefour_options_t options;

   memset(&global_stats, 0, sizeof(struct carrefour_global_stats));

   printk("max_lin_address = %lx\n", max_lin_address);
   printk("min_lin_address = %lx\n", min_lin_address);

   printk("NPPT  = Nr processed page touched\n"); 
   printk("NSAAO = Nr samples after an order\n"); 
   printk("TNO   = Total nr of orders\n"); 
   printk("TNSIT = Total number samples in the tree\n"); 
   printk("TNSM  = Total number sample missed\n"); 

   // Module options
   if(! validate_module_options()) {
      printk("Invalid options\n");
      return -1;
   }

   print_module_options();
   //

   err = ibs_init();

   if(err) {
      return err;
   }

   ibs_create_procs_files();
   machine_init();

   rbtree_load_module();

   reset_carrefour_hooks();

   memset(&options, 0, sizeof(struct carrefour_options_t));
   options.page_bouncing_fix_4k = carrefour_module_options[PAGE_BOUNCING_FIX_4K].value;
   options.page_bouncing_fix_2M = carrefour_module_options[PAGE_BOUNCING_FIX_2M].value;
   options.async_4k_migrations  = 1;
   //options.throttle_2M_migrations_limit = 5;
   configure_carrefour_hooks(options);

   printk("Carrefour hooks options\n\tpage_bouncing_fix_4k = %d\n\tpage_bouncing_fix_2M = %d\n\tasync_4k_migrations = %d, thottle (2M = %d, 4k = %d)\n",
      options.page_bouncing_fix_4k, options.page_bouncing_fix_2M, options.async_4k_migrations, options.throttle_2M_migrations_limit, options.throttle_4k_migrations_limit);
 
   thp_initial_state = get_thp_state();
   
   printu("THP state = %s\n", (thp_initial_state == THP_ALWAYS) ? "always" : ((thp_initial_state == THP_MADVISE) ? "madvise" : "disabled"));
   return 0;
}

static void __exit carrefour_exit_module(void) {
   if(running) {
      enable_carrefour = 0;
      stop_profiling();
      running = 0;
   }

   ibs_exit();
   ibs_remove_proc_files();

   rbtree_remove_module();

   set_thp_state(thp_initial_state);

   printu("[GLOBAL] %% shared pages = %lu %%, Nr hot pages = %lu , %% access most accessed page = %lu %%\n", 
          global_stats.total_access? (global_stats.total_access_shared_page * 100 / global_stats.total_access) : 0, global_stats.max_nr_hot_pages, global_stats.max_percent_page);


   printk(KERN_INFO "sdp: shutdown\n");
}

module_init(carrefour_init_module);
module_exit(carrefour_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Baptiste Lepers <baptiste.lepers@inria.fr> and Aleksey Pesterev <alekseyp@mit.edu>");
MODULE_DESCRIPTION("Kernel IBS Interleaver Module");

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

extern unsigned sampling_rate;
#if ADAPTIVE_SAMPLING
extern unsigned sampling_rate_cheap;
extern unsigned sampling_rate_accurate;
#endif
extern unsigned long nr_accesses_node[MAX_NUMNODES];

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
#if ADAPTIVE_SAMPLING
         //printk("[ADAPTIVE] Carrefour disabled, reducing the IBS sampling rate\n");
         sampling_rate = sampling_rate_cheap;
#endif
         start_profiling();
      }
      else if (c == 'k' && running) {
         enable_carrefour = 0;
         stop_profiling();
         running = 0;
      }
#if ENABLE_INTERLEAVING
      else if (c == 'i') {
         enable_interleaving = 0;
      }
      else if (c == 'I') {
         enable_interleaving = 1;
      }
#endif
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
                     memset(&nr_accesses_node, 0, sizeof(unsigned long) * MAX_NUMNODES);
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
#if ENABLE_REPLICATION && REPLICATION_PER_TID
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
#elif ENABLE_REPLICATION
      else if (c == 'r') {
         enable_replication = 0;
      }
      else if (c == 'R') {
         enable_replication = 1;
      }
#endif

#if ENABLE_MIGRATION
      else if (c == 'M') {
         enable_migration = 1;
      }
      else if (c == 'm') {
         enable_migration = 0;
      }
#endif

#if ADAPTIVE_SAMPLING
      else if (c == 'F') {
         // Increases the ibs frequency
         sampling_rate = sampling_rate_accurate;
      }
      else if (c == 'f') {
         // Decreases the ibs frequency
         sampling_rate = sampling_rate_cheap;
      }
#endif
   }
   return count;
}

static const struct file_operations ibs_cntrl_fops = {
   .write          = ibs_proc_write,
};

void ibs_create_procs_files(void) {
   proc_create("inter_cntl", S_IWUGO, NULL, &ibs_cntrl_fops);
}

void ibs_remove_proc_files(void) {
   remove_proc_entry("inter_cntl", NULL);
}

/** What to do when starting profiling **/
/** Must be called with NMI disabled   **/
extern int consider_L1L2;
#if DUMP_OVERHEAD
static u64 time_start_profiling, time_before_stop_profiling, time_after_stop_profiling;
extern u64 time_spent_in_NMI;
extern u64 time_spent_in_migration;
#endif

int start_profiling(void) {
#if DUMP_OVERHEAD
   time_spent_in_migration = 0;
   rdtscll(time_start_profiling);
#endif

   rbtree_init();
#if ENABLE_THREAD_PLACEMENT
   tids_init();
#endif
   carrefour_init();

   consider_L1L2 = 1;
   ibs_start();
   return 0;
} 

/** And when stoping profiling         **/
/** Must be called with NMI disabled   **/
#if AGGRESSIVE_FIX
int disable_state = 0;
#endif

int permanently_disable_carrefour=0;
int stop_profiling(void) {
#if DUMP_OVERHEAD
   rdtscll(time_before_stop_profiling);
#endif
   
   //rbtree_print(); //debug
   //show_tid_sharing_map(); //debug

   if(permanently_disable_carrefour || (!enable_replication && !enable_interleaving && !enable_migration)) {
      enable_carrefour = 0;
   }

   //printk("Current processor %d\n", smp_processor_id());
#if ADAPTIVE_SAMPLING
   printk("-- Carrefour %s, replication %s, interleaving %s, migration %s, frequency %s\n", 
            enable_carrefour    ? "enabled" : "disabled",
            enable_replication  ? "enabled" : "disabled",
            enable_interleaving ? "enabled" : "disabled",
            enable_migration    ? "enabled" : "disabled",
            sampling_rate == sampling_rate_accurate ? "accurate" : "cheap");
#else
   printk("-- Carrefour %s, replication %s, interleaving %s, migration %s\n", 
            enable_carrefour    ? "enabled" : "disabled",
            enable_replication  ? "enabled" : "disabled",
            enable_interleaving ? "enabled" : "disabled",
            enable_migration    ? "enabled" : "disabled");
#endif
   ibs_stop();

#if DETAILED_STATS
   if(!permanently_disable_carrefour) {
      carrefour();
   }
#else 
   if(enable_carrefour) {
      carrefour();
   }
#endif
   
   /** free all memory **/
   rbtree_clean();
#if ENABLE_THREAD_PLACEMENT
   tids_clean();
#endif
   carrefour_clean();

#if DUMP_OVERHEAD
   rdtscll(time_after_stop_profiling);
   if(num_online_cpus() > 0 && (time_after_stop_profiling - time_start_profiling > 0)) {
      unsigned long total_time = time_after_stop_profiling - time_start_profiling;
      unsigned long stop_profiling_time = time_after_stop_profiling - time_before_stop_profiling;

      printk("-- Carrefour %lu total profiling time, %lu stop_profiling, %lu in NMI - Overhead master core %d%%, average overhead %d%%\n",
            (unsigned long) total_time,
            (unsigned long) stop_profiling_time,
            (unsigned long) time_spent_in_NMI,
            (int)(stop_profiling_time * 100 / total_time),
            (int)((time_spent_in_NMI / num_online_cpus()) * 100 / total_time)
            );

      printk("-- Carrefour %lu total migration time, migration overhead (%d%%)\n",
            (unsigned long) time_spent_in_migration,
            (int) (time_spent_in_migration * 100 / (time_after_stop_profiling - time_start_profiling))
            );

   }

   printk("Current core is %d\n", smp_processor_id());
#endif

   return 0;
}

/**
 * Init and exit
 */
extern unsigned long min_lin_address;
extern unsigned long max_lin_address;
extern unsigned sampling_rate;
module_param(min_lin_address, ulong, S_IRUGO);
module_param(max_lin_address, ulong, S_IRUGO);
module_param(sampling_rate, uint, S_IRUGO);

#if ADAPTIVE_SAMPLING
extern unsigned sampling_rate_accurate;
extern unsigned sampling_rate_cheap;
module_param(sampling_rate_accurate, uint, S_IRUGO);
module_param(sampling_rate_cheap, uint, S_IRUGO);
#endif

extern struct carrefour_global_stats global_stats;

static int __init carrefour_init_module(void) {
   int err;

   memset(&global_stats, 0, sizeof(struct carrefour_global_stats));

   printk("max_lin_address = %lx\n", max_lin_address);
   printk("min_lin_address = %lx\n", min_lin_address);

#if ADAPTIVE_SAMPLING
   sampling_rate = sampling_rate_accurate;
#endif

   printk("sampling_rate   = %x\n", sampling_rate);

#if ADAPTIVE_SAMPLING
   printk("sampling_rate_cheap    = %x\n", sampling_rate_cheap);
   printk("sampling_rate_accurate = %x\n", sampling_rate_accurate);
#endif

   printk("NPPT  = Nr processed page touched\n"); 
   printk("NSAAO = Nr samples after an order\n"); 
   printk("TNO   = Total nr of orders\n"); 
   printk("TNSIT = Total number samples in the tree\n"); 
   printk("TNSM  = Total number sample missed\n"); 

   err = ibs_init();

   if(err) {
      return err;
   }

   ibs_create_procs_files();
   machine_init();

   return 0;
}

static void __exit carrefour_exit_module(void) {
   ibs_exit();
   ibs_remove_proc_files();

   printk(KERN_INFO "sdp: shutdown\n");
}

module_init(carrefour_init_module);
module_exit(carrefour_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Baptiste Lepers <baptiste.lepers@inria.fr> and Aleksey Pesterev <alekseyp@mit.edu>");
MODULE_DESCRIPTION("Kernel IBS Interleaver Module");

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

/** rbtree to hold seen pages **/
/** Currently it is protected by a big lock **/
struct rb_root pagetree;
DEFINE_SPINLOCK(pagetree_lock);

/** We preallocate rbtree nodes; this has 2 advantages:
 *  - speed
 *  - no need to free things at the end of profiling, just reset the index to 0 :)
 */

struct page_reserve pagesreserve;

/** RBTREE stats **/
DEFINE_PER_CPU(struct rbtree_stats_t, rbtree_core_stats);

/** init **/
extern unsigned sampling_rate;
#if ADAPTIVE_SAMPLING
extern unsigned sampling_rate_accurate;
extern unsigned sampling_rate_cheap;
#endif

void rbtree_init(void) {
   int cpu;
   unsigned size;
   static int first_time = 1;

   if(first_time) {
      size = MAX_PAGES_TO_WATCH_ACCURATE * sizeof(struct sdpage);
      first_time = 0;
   }
   else {
      size = pagesreserve.index * sizeof(struct sdpage); 
   }

   pagetree = RB_ROOT;
   pagesreserve.index = 0;

   for_each_online_cpu(cpu) {
      memset(&per_cpu(rbtree_core_stats, cpu), 0, sizeof(struct rbtree_stats_t));
   }

#if ADAPTIVE_SAMPLING
   if(sampling_rate == sampling_rate_accurate) {
      pagesreserve.max_pages_to_watch = MAX_PAGES_TO_WATCH_ACCURATE;
   }
   else {
      pagesreserve.max_pages_to_watch = MAX_PAGES_TO_WATCH_CHEAP;
   }
#else
   pagesreserve.max_pages_to_watch = MAX_PAGES_TO_WATCH_ACCURATE;
#endif

   memset(pagesreserve.pages, 0, size);

   printk("Max # of pages = %u (size = %lu ko)\n", pagesreserve.max_pages_to_watch, pagesreserve.max_pages_to_watch * sizeof(struct sdpage) / 1024);
   //printk("Tree address: %p (phys = 0x%lu, node = %d)\n:\n", pagesreserve.pages, virt_to_phys(pagesreserve.pages), phys2node(virt_to_phys(pagesreserve.pages)));
   spin_lock_init(&pagetree_lock);
}

void rbtree_get_merged_stats(struct rbtree_stats_t * stats_to_fill, struct carrefour_run_stats * c_stats) {
   int cpu;
   memset(stats_to_fill, 0, sizeof(struct rbtree_stats_t));

   for_each_online_cpu(cpu) {
      struct rbtree_stats_t* stat = &per_cpu(rbtree_core_stats, cpu);
      stats_to_fill->nr_ld_samples += stat->nr_ld_samples;
      stats_to_fill->nr_st_samples += stat->nr_st_samples;
      stats_to_fill->total_samples_in_tree += stat->total_samples_in_tree;
      stats_to_fill->total_samples_missed += stat->total_samples_missed;
      stats_to_fill->nr_pages_in_tree += stat->nr_pages_in_tree;
   }

   c_stats->avg_nr_samples_per_page = stats_to_fill->nr_pages_in_tree ? (double) stats_to_fill->total_samples_in_tree / (double) stats_to_fill->nr_pages_in_tree : 0;
}

/** rbtree insert; for some reason the kernel does not provide an implem... */
static struct sdpage * insert_in_page_rbtree(struct rb_root *root, struct sdpage *data, int add) {
   struct rb_node **new = &(root->rb_node), *parent = NULL;

   /* Figure out where to put new node */
   while (*new) {
      struct sdpage *this = container_of(*new, struct sdpage, node);
      parent = *new;
      if (data->page_phys > this->page_phys)
         new = &((*new)->rb_left);
      else if (data->page_phys < this->page_phys)
         new = &((*new)->rb_right);
      else
         return this;
   }

   /* Add new node and rebalance tree. */
   if(add) {
      rb_link_node(&data->node, parent, new);
      rb_insert_color(&data->node, root);
      return data;
   } else {
      return NULL;
   }
}


/** Called on all IBS samples. Put a page in the rbtree **/
extern unsigned long nr_of_samples_after_order;
void rbtree_add_sample(int is_kernel, struct ibs_op_sample *ibs_op, int cpu, int pid, int tgid) {
   unsigned long flags;
   struct page_reserve *r = &pagesreserve;
   struct rb_root *root = &pagetree;
   struct sdpage *tmp, *tmp2;

   struct rbtree_stats_t* core_stats;

   int is_softirq = 0;  //WE NEED TO FIND A WAY TO KNOW THAT WE ARE IN IRQ.
                        //I have a kernel hack that does that but can it be done without modifying the kernel?
   //int is_softirq = get_is_in_soft_irq(cpu);
#if ENABLE_THREAD_PLACEMENT
   int tid_index = 0;
#endif
   int node = cpu_to_node(cpu);

   int new = 0;

   /*** Do not consider pages acccessed by the kernel or in IRQ context ***/
   if(is_kernel || is_softirq)
      return;
   if(tgid == 0)
      return;

   /* 
      local_irq_save(flags);
      preempt_disable();
      if(page_has_already_been_treated(pid, (ibs_op->lin_addr & SDPAGE_MASK)) == 1) {
      nr_of_samples_after_order++;
      }
      local_irq_restore(flags);
      preempt_enable();
   */

   /*** Insert in rbtree ***/
   spin_lock_irqsave(&pagetree_lock, flags);
#if ENABLE_THREAD_PLACEMENT
   tid_index = get_tid_index(pid);
#endif

   tmp = &r->pages[r->index];
   tmp->page_lin = (void *) (ibs_op->lin_addr & SDPAGE_MASK);
   tmp->page_phys = (void *) (ibs_op->phys_addr & SDPAGE_MASK);
   tmp->tgid = tgid;

   tmp2 = insert_in_page_rbtree(root, tmp, (r->index < pagesreserve.max_pages_to_watch - 1));
   if(tmp2 == tmp && r->index < pagesreserve.max_pages_to_watch - 1) {
      r->index++;
      new = 1;
   }

   spin_unlock_irqrestore(&pagetree_lock, flags);

   core_stats = get_cpu_ptr(&rbtree_core_stats);
   if(tmp2) {
#if ENABLE_THREAD_PLACEMENT
#warning Not really thread safe
      if(tid_index < MAX_PIDS_TO_WATCH) {
	      SET_TID(tmp2->tids, tid_index);
	      increment_tid_counts(tid_index, tmp2->tids);
	      increment_tid_node_access(tid_index, phys2node(ibs_op->phys_addr));
      } else {
         WARN_ONCE(tid_index >= MAX_PIDS_TO_WATCH, "Strange tid index: %d\n", tid_index);
      }
#endif

      if(node >= num_online_nodes() || node < 0) {
         printk("Strange node %d\n", node);
      } else {
         __sync_fetch_and_add(&(tmp2->nb_accesses[node]), 1);
      }

      if(ibs_op->data3.IbsStOp) { 
         __sync_fetch_and_add(&(tmp2->nb_writes), 1);
      }
      
      core_stats->total_samples_in_tree++;
      if(new) {
         core_stats->nr_pages_in_tree++;
      }
   }
   else {
      core_stats->total_samples_missed++;
   }

   if(ibs_op->data3.IbsStOp) { 
      core_stats->nr_st_samples ++;
   }
   if(ibs_op->data3.IbsLdOp) { 
      core_stats->nr_ld_samples ++;
   }

   put_cpu_ptr(&rbtree_core_stats);

err: __attribute__((unused));
   return;
}


void rbtree_clean(void) {
   printk("Before cleaning: %d pages in the rbtree\n", pagesreserve.index);
}


/** Print rbree - for debug purpose **/
static void _print_page_rbtree(struct rb_node **new) {
   if (*new) {
      struct sdpage *this = container_of(*new, struct sdpage, node);
      _print_page_rbtree(&((*new)->rb_left));
      printk("Page: %15lx %15lx [%d %d %d %d] [%d]\n", (long unsigned)this->page_phys, (long unsigned)this->page_lin,
            this->nb_accesses[0], this->nb_accesses[1], this->nb_accesses[2], this->nb_accesses[3],
            this->tgid);
      _print_page_rbtree(&((*new)->rb_right));
   }
}
void rbtree_print(void) {
   unsigned long flags;
   struct rb_node **new;

   spin_lock_irqsave(&pagetree_lock, flags);
   new = &(pagetree.rb_node);
   _print_page_rbtree(new);
   spin_unlock_irqrestore(&pagetree_lock, flags);
}


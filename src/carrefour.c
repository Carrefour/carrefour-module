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
#include <asm/processor.h>
#include <linux/kernel.h>

struct carrefour_run_stats    run_stats;
struct carrefour_global_stats global_stats;

#if ENABLE_THREAD_PLACEMENT
static void decide_threads_placement(void); 
#endif

static void decide_pages_fate(void); 

static int phase = 0;

#if ADAPTIVE_SAMPLING
extern unsigned sampling_rate;
extern unsigned sampling_rate_accurate;
extern unsigned sampling_rate_cheap;
static unsigned sampling_rate_switch_magic = 10;
#endif

#if ENABLE_REPLICATION
unsigned enable_replication = 1;
#else
unsigned enable_replication = 0;
#endif

// The first time, to enable replication we need at least 100 orders
// because replicating a page is costly (creation of multiple pgd)
// Set to 1, if replication has already been enable once
// TODO: do it per PID
unsigned min_nr_orders_enable_replication = 500;
unsigned min_nr_samples_for_migration = 2; // Only active if interleaving is disabled. 1 otherwise.


#if ENABLE_INTERLEAVING
unsigned enable_interleaving = 1;
#else
unsigned enable_interleaving = 0;
#endif

#if ENABLE_MIGRATION
unsigned enable_migration = 1;
#else
unsigned enable_migration = 0;
#endif

int enable_carrefour;

unsigned long nr_accesses_node[MAX_NUMNODES];
unsigned long interleaving_distrib[MAX_NUMNODES];
unsigned long interleaving_distrib_max = 0;
unsigned long interleaving_proba[MAX_NUMNODES];

void carrefour(void) {

#if ENABLE_THREAD_PLACEMENT 
   if(phase % 2 == 0) {
      decide_threads_placement();
   } 
#else
   phase = 1;
#endif

   if(phase % 2 == 1) { //Not an else if because decide_threads_placement might change phase!
      decide_pages_fate();

#if ADAPTIVE_SAMPLING
      {
         if((run_stats.total_nr_orders < sampling_rate_switch_magic)) {
            //printk("[ADAPTIVE] Did not take enough decision (%d). Reducing the IBS sampling rate\n", nr_decisions);
            sampling_rate = sampling_rate_cheap;
         }
         else if ((run_stats.total_nr_orders >= sampling_rate_switch_magic)) {
            //printk("[ADAPTIVE] Took lots of decisions (%d). Increasing the IBS sampling rate\n", nr_decisions);
            sampling_rate = sampling_rate_accurate;
         }
      }
#endif

   }

   phase++;
}


#if ENABLE_THREAD_PLACEMENT
/**
 * Thread migration code 
 **/
static void move_thread(pid_t tid, int node) {
   int ret;
   printk("#Moving %d to %d\n", tid, node);
   ret = sched_setaffinity_hook(tid, cpumask_of_node(node));
   sched_setaffinity_hook(tid, cpu_online_mask);
   if(ret)
      run_stats.nb_failed_thr_migration++;
   else
      run_stats.nb_thr_migration++;
}

static void move_threads(int nb_clusters, int* clusters) {
   int i;
   int nb_tids = tids_nb_seen_tids();
   int *nodes = compute_clusters_best_node(nb_clusters, clusters);
   int expected_node_nb_thread[MAX_NUMNODES];
   int nb_cores_per_node = get_nb_cores_per_node();

   int max_nb_thr_per_cluster = 0;
   memset(expected_node_nb_thread, 0, sizeof(expected_node_nb_thread));
   for(i = 0; i < nb_tids; i++) {
      int nb = ++expected_node_nb_thread[nodes[clusters[i]]];
      if(nb > max_nb_thr_per_cluster)
         max_nb_thr_per_cluster = nb;
   }

   // First check: we don't even need to do extra work if the #threads / node is low
   if(max_nb_thr_per_cluster < nb_cores_per_node && nb_clusters <= num_online_nodes()) {
	   printk("#Short path: low number of threads\n");
	   for(i = 0; i < nb_tids; i++) {
	      move_thread(get_tid(i), nodes[clusters[i]]);
	   }
   } else {
	   u64 *weight;
	   int *ignored_clusters;

	   if(nb_clusters == nb_tids) {
		   printk("#Short path 2: clusters are useless (as many clusters as threads)\n");
		   goto end;
	   }

	   weight = compute_clusters_weight(nb_clusters, clusters);
	   ignored_clusters = kmalloc(sizeof(*ignored_clusters)*nb_clusters, GFP_KERNEL);
	   
	   for(i = 0; i < nb_clusters; i++) {
		   if(weight[i] > nb_cores_per_node*100L*100L) { // Load is too high for a single node, ignore
			   printk("#Ignore cluster %d (load is too high %llu)\n", i, (long long unsigned)(weight[i]));
			   ignored_clusters[i] = 1;
		   } else {
			   ignored_clusters[i] = 0;
		   }
	   }

	   for(i = 0; i < nb_tids; i++) {
		   if(!ignored_clusters[clusters[i]])
			   move_thread(get_tid(i), nodes[clusters[i]]);
	   }

	   kfree(weight);
	   kfree(ignored_clusters);
   }
end:
   kfree(nodes);
}

void decide_threads_placement(void) {
   int *clusters = tids_compute_clusters();
   int nb_clusters = simplify_clusters(clusters);

   printk("#Found %d clusters\n", nb_clusters);
   move_threads(nb_clusters, clusters);

   kfree(clusters);

   printk("Phase %d: Carrefour - %d thread migration %d failed migrations\n",
         phase, run_stats.nb_thr_migration, run_stats.nb_failed_thr_migration);

   /* If we did not suceed to migrate threads, fall back to memory migration */
   if(run_stats.nb_thr_migration == 0)
	   phase++;
}
#endif

/****
 * Algorithm plumbery for page migration / replication
 * Store pages in appropriate structs
 ****/


/* Each pid has a few struct pid_pages. These structs basically store the pages accessed by a pid.
 * We group pages by pid in order to do batch migration/replication (otherwise we would have to find the vma each time */
struct pid_pages {
   struct pid_pages *next;
   int nb_pages;
   int nb_max_pages;
   pid_t tgid;
   void **pages;
   int *nodes;
};
/* Two structs : pages to be interleaved/migrated and pages to be replicated */
/* Contains pid_pages structs */
static struct pages_container {
   int nb_pid;
   struct pid_pages *pids;
} pages_to_interleave, pages_to_replicate;

struct pid_pages *insert_pid_in_container(struct pages_container *c, pid_t tgid) {
   struct pid_pages *p = kmalloc(sizeof(*p), GFP_KERNEL);
   memset(p, 0, sizeof(*p));
   p->tgid = tgid;
   p->next = c->pids;
   c->pids = p;
   c->nb_pid++;
   return p;
}

void insert_page_in_container(struct pages_container *c, pid_t tgid, void *page, int node) {
   struct pid_pages *p = NULL;
   struct pid_pages *l = c->pids;
   while(l) {
      if(l->tgid == tgid) {
         p = l;
         break;
      }
      l = l->next;
   }

   if(!p) 
      p = insert_pid_in_container(c, tgid);
   
   if(p->nb_pages >= p->nb_max_pages) {
      if(p->nb_max_pages) {
         p->nb_max_pages *= 2;
      } else {
         p->nb_max_pages = 256;
      }
      p->pages = krealloc(p->pages, sizeof(*p->pages)*p->nb_max_pages, GFP_KERNEL);
      p->nodes = krealloc(p->nodes, sizeof(*p->nodes)*p->nb_max_pages, GFP_KERNEL);
   }
   p->pages[p->nb_pages] = page;
   p->nodes[p->nb_pages] = node;
   p->nb_pages++;
}

static void carrefour_free_pages(struct pages_container *c) {
   struct pid_pages *p, *tmp;
   for(p = c->pids; p;) {
      if(p->pages)
         kfree(p->pages);
      if(p->nodes)
         kfree(p->nodes);
      tmp = p;
      p = p->next;
      kfree(tmp);
   }
   c->pids = 0;
   c->nb_pid = 0;
}



/***
 * Actual interesting stuff 
 ***/
static void decide_page_fate(struct sdpage *this) {
   int nb_dies = 0, dest_node = -1;
   int i;

   int should_be_interleaved = 0;
   int should_be_migrated = 0;
   int should_be_replicated = 0;

   unsigned long total_nb_accesses = 0;
   unsigned long from_node;
#if DETAILED_STATS
   int page_status = 0;
#endif

   for(i = 0; i < num_online_nodes(); i++) {
      if(this->nb_accesses[i] != 0) {
         nb_dies++;
         dest_node = i;
      }
      total_nb_accesses += this->nb_accesses[i];
   }

#if PREDICT_WITH_STDDEV
   run_stats.stddev_nr_samples_per_page +=  ( total_nb_accesses - run_stats.avg_nr_samples_per_page) * ( total_nb_accesses - run_stats.avg_nr_samples_per_page ); 
#endif

#if DETAILED_STATS
   page_status = page_has_already_been_treated(this->tgid, (unsigned long) this->page_lin);
   if(page_status == 1) {
      run_stats.nr_of_samples_after_order   += total_nb_accesses;
      run_stats.nr_of_process_pages_touched ++;
   }

   if(page_status || !enable_carrefour) {
      return;
   }
#endif


   from_node = phys2node((unsigned long) this->page_phys);

   /*****
    * THIS IS THE REAL CORE AND IMPORTANT LOGIC
    * - duplicate pages accessed from multiple nodes in RO
    * - interleave pages accessed from multiple nodes in RW
    * - move pages accessed from 1 node
    ****/
#if ENABLE_REPLICATION && REPLICATION_PER_TID
   if(enable_replication &&  is_allowed_to_replicate(this->tgid) && (nb_dies > 1) && ((this->nb_writes == 0) && is_user_addr(this->page_lin))) {
      should_be_replicated = 1;
   }
#elif ENABLE_REPLICATION
    if(enable_replication && (nb_dies > 1) && ((this->nb_writes == 0) && is_user_addr(this->page_lin))) {
      should_be_replicated = 1;
   }
#endif

#if ENABLE_INTERLEAVING
   if(enable_interleaving && (nb_dies > 1)) {
      should_be_interleaved = 1;
   }
#endif

#if ENABLE_MIGRATION
   if(((enable_migration && nb_dies == 1 && total_nb_accesses >= min_nr_samples_for_migration) || enable_interleaving) && from_node != dest_node) {
      should_be_migrated = 1;
   }
#endif

   /** The priority is to replicate if possible **/
   if(should_be_replicated) {
      //printk("Choosing replication for page %p:%d (total_nb_accesses = %lu, nb_dies = %d)\n", this->page_lin, this->tgid, total_nb_accesses, nb_dies);
      insert_page_in_container(&pages_to_replicate, this->tgid, this->page_lin, 0);
      run_stats.nr_requested_replications++;
   }
   else if (should_be_interleaved) {
      int rand = random32()%101; /** Between 0 and 100% **/

#if !REPLICATION_PER_TID
      if(rand <= interleaving_proba[from_node]) {
         rand = random32()%interleaving_distrib_max;
         for(dest_node = 0; interleaving_distrib[dest_node] < rand; dest_node++) {}

         if(dest_node >= num_online_nodes()) {
            printk("[WARNING] Bug ...\n");
         }
         else if (dest_node != from_node) {
            insert_page_in_container(&pages_to_interleave, this->tgid, this->page_lin, dest_node);
            run_stats.nb_interleave_orders++;
            //printk("Will interleave page on node %d\n", dest_node);
         }
      }
#else
      // This is a quick and dirty hack for now. 
      // Prevents interleaving a page on a node where there are no threads of this pid.
      // It only works because each application has two nodes on our setup.
      // To be corrected.
      if(rand <= interleaving_proba[from_node]) {
         for(dest_node = 0; dest_node < num_online_nodes(); dest_node++) {
            if(this->nb_accesses[dest_node] != 0 && dest_node != from_node) {
               break;
            }
         }

         if(dest_node >= num_online_nodes()) {
            printk("[WARNING] Bug ...\n");
         }
         else {
            insert_page_in_container(&pages_to_interleave, this->tgid, this->page_lin, dest_node);
            run_stats.nb_interleave_orders++;
         }
      }
#endif
   }
   else if (should_be_migrated) { 
      insert_page_in_container(&pages_to_interleave, this->tgid, this->page_lin, dest_node);
      run_stats.nb_migration_orders++;
   }
}

void decide_pages_fate(void) {
   int i,j;
   struct pid_pages *p;
#if ENABLE_INTERLEAVING
   unsigned long total_accesses = 0;
#endif

#if AGGRESSIVE_FIX
   double carrefour_efficiency = 0;
#endif

   struct rbtree_stats_t rbtree_stats;
   rbtree_get_merged_stats(&rbtree_stats, &run_stats);

#if ENABLE_INTERLEAVING
   /* Compute the imbalance and set up the right probabilities for interleaving */
   for(i = 0; i < num_online_nodes(); i++) { 
      //printk("Load on node %d : %lu memory accesses\n", i, nr_accesses_node[i]);
      total_accesses += nr_accesses_node[i];
   }

   for(i = 0; enable_interleaving && i < num_online_nodes(); i++) {
      if(total_accesses) {
         interleaving_proba[i] = (nr_accesses_node[i]*100 / total_accesses);
         interleaving_distrib[i] = interleaving_distrib_max + (100 - interleaving_proba[i]);
         interleaving_distrib_max = interleaving_distrib[i];
         //printk("Interleaving distrib node %d : %lu\n", i, interleaving_distrib[i]);
      }
      else {
         interleaving_proba[i] = 100;
         interleaving_distrib[i] = interleaving_distrib_max + 100;
         interleaving_distrib_max = interleaving_distrib[i];

         printk("Warning: total_accesses = 0\n");
      }
   }
#endif

   for(i = 0; i < pagesreserve.index; i++) { 
      decide_page_fate(&pagesreserve.pages[i]);
   }

#if PREDICT_WITH_STDDEV
   run_stats.stddev_nr_samples_per_page = rbtree_stats.nr_pages_in_tree ? 
      (double) int_sqrt((unsigned long) ((run_stats.stddev_nr_samples_per_page / rbtree_stats.nr_pages_in_tree) * 100. / run_stats.avg_nr_samples_per_page)) : 0;

   if(run_stats.stddev_nr_samples_per_page > STDDEV_THRESHOLD) {
      goto skip_carrefour;
   } 
#endif

   /* Migrate or interleave */
   for(p = pages_to_interleave.pids; p; p = p->next) {
      int err = s_migrate_pages(p->tgid, p->nb_pages, p->pages, p->nodes, NULL, MPOL_MF_MOVE_ALL);
      //printk("Moving %d pages of pid %d!\n", p->nb_pages, p->tgid);

      if(err) {
         switch(err) {
            case -ENOMEM:
               printk("!No memory left to perform page migration\n");
               break;
            case -ESRCH:
               printk("!Cannot migrate tasks of pid %d because it does not exist!\n", p->tgid);
               break;
            case -EINVAL:
               printk("!Cannot migrate tasks of pid %d: no mm?!\n", p->tgid);
               break;
            default:
               printk("!Migration returned error %d\n", err);
               break;
         }
      }
   }

   if(run_stats.real_nb_migrations) {
      for(i = 0; i < num_online_nodes(); i++) {
         printk("Moving pages from node %d: ", i);
         for(j = 0; j < num_online_nodes(); j++) {
            printk("%d\t", run_stats.migr_from_to_node[i][j]);
         }
         printk("\n");
      }
   }

   /* replicate */
#if ENABLE_REPLICATION
   if(run_stats.nr_requested_replications >= min_nr_orders_enable_replication) {
      //printk("Nr requested replication = %u\n", nr_requested_replications); 

      min_nr_orders_enable_replication = 1;
      for(p = pages_to_replicate.pids; p; p = p->next) {
         int err;
         int i;

         /** TODO:
          * try to "merge" individual pages in groups of contiguous pages to reduce the number of calls
          * thus the number of VMA creation
          * 
          * We should really be careful with tgids because a tgid might have disappeared or worse been reallocated !
          **/
         for(i = 0; i < p->nb_pages; i++) {
            //printk("Replicating page 0x%lx (user = %d)\n", (unsigned long) p->pages[i], is_user_addr(p->pages[i]));
            err = replicate_madvise(p->tgid, (unsigned long) p->pages[i], PAGE_SIZE, MADV_REPLICATE); 
            if(err) {
               printk("!Cannot replicate page %p\n", NULL);
            }
            else {
               run_stats.nb_replication_orders++;
            }
         }

         //enable_interleaving = 0;
      }
   }
#endif

#if PREDICT_WITH_STDDEV
skip_carrefour:
#endif
   /* Clean the mess */
   carrefour_free_pages(&pages_to_interleave);
   carrefour_free_pages(&pages_to_replicate);

   run_stats.total_nr_orders = run_stats.real_nb_migrations + run_stats.nb_replication_orders;
   global_stats.total_nr_orders += run_stats.total_nr_orders;

   global_stats.cumulative_real_nb_migrations += run_stats.real_nb_migrations;
   global_stats.cumulative_nb_replication_orders += run_stats.nb_migration_orders;
   global_stats.cumulative_nb_replication_orders += run_stats.nb_replication_orders;
   global_stats.cumulative_nb_interleave_orders += run_stats.nb_interleave_orders; 

   printk("Phase %d: Carrefour - %d migration %d interleaving %d replication ( %u real migrations)\n",
         phase, run_stats.nb_migration_orders, run_stats.nb_interleave_orders, run_stats.nb_replication_orders, run_stats.real_nb_migrations
         );

#if PREDICT_WITH_STDDEV
   printk("%lu pages, %lu samples, avg = %lu, stddev = %lu %%\n", 
         rbtree_stats.nr_pages_in_tree, rbtree_stats.total_samples_in_tree,
         (unsigned long) run_stats.avg_nr_samples_per_page, (unsigned long) run_stats.stddev_nr_samples_per_page);
#else
   printk("%lu pages, %lu samples, avg = %lu\n", 
         rbtree_stats.nr_pages_in_tree, rbtree_stats.total_samples_in_tree,
         (unsigned long) run_stats.avg_nr_samples_per_page);
#endif

#if DETAILED_STATS
   printk("NPPT = %lu -- NSAAO = %lu -- TNO = %lu -- TNSIT = %lu -- TNSM = %lu\n", 
      run_stats.nr_of_process_pages_touched, run_stats.nr_of_samples_after_order, global_stats.total_nr_orders, rbtree_stats.total_samples_in_tree, rbtree_stats.total_samples_missed);

#if AGGRESSIVE_FIX
   carrefour_efficiency = ((double) run_stats.nr_of_samples_after_order / (double) rbtree_stats.total_samples_in_tree);
   if(carrefour_efficiency < .75) {
      disable_state++;
      if(disable_state > 3) {
         permanently_disable_carrefour = 1;
      }
   }
   else {
      disable_state = 0;
   }
#endif
#endif
}


void carrefour_init(void) {
   memset(&run_stats, 0, sizeof(struct carrefour_run_stats));

   memset(&pages_to_interleave, 0, sizeof(pages_to_interleave));
   memset(&pages_to_replicate, 0, sizeof(pages_to_replicate));
   memset(&nr_accesses_node, 0, sizeof(unsigned long) * MAX_NUMNODES);
   memset(&interleaving_distrib, 0, sizeof(unsigned long) * MAX_NUMNODES);
   interleaving_distrib_max = 0;
   memset(&interleaving_proba, 0, sizeof(unsigned long) * MAX_NUMNODES);
}

void carrefour_clean(void) {
}


int page_has_already_been_treated(int pid, unsigned long addr) {
   struct task_struct *task;
   struct mm_struct *mm = NULL;
   struct vm_area_struct *vma;
   struct page * page;
   int ret = -1;
   int err;

   rwlock_t *lock = (rwlock_t*)(void*)tasklist_lock_hook;

   //printk("[Core %d, PID %d] Acquiring task lock (0x%p)\n", smp_processor_id(), pid, lock);
   read_lock(lock);
   task = find_task_by_vpid_hook(pid);
   if(task) {
      mm = get_task_mm(task);
   }

   //printk("[Core %d, PID %d] Releasing task lock (0x%p)\n", smp_processor_id(), pid, lock);
   read_unlock(lock);
   if (!mm)
      return -1;

   
   //printk("[Core %d, PID %d] Acquiring mm lock (0x%p)\n", smp_processor_id(), pid, &mm->mmap_sem);
   down_read(&mm->mmap_sem);

   vma = find_vma(mm, addr);
   if (!vma || addr < vma->vm_start)
      goto out_locked;

   page = follow_page_hook(vma, addr, FOLL_GET);

   err = PTR_ERR(page);
   if (IS_ERR(page) || !page)
      goto out_locked;


   if(page->stats.nr_migrations) { // Page has been migrated already once
      ret = 1;
   }
#if ENABLE_REPLICATION
   else if(PageReplication(page)) {
      ret = 1;
   }
#endif
   else {
      ret = 0;
   }

   put_page(page);

out_locked:
   //printk("[Core %d, PID %d] Releasing mm lock (0x%p)\n", smp_processor_id(), pid, &mm->mmap_sem);
   up_read(&mm->mmap_sem);
   mmput(mm);

   return ret;
}

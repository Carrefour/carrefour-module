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
#include <linux/hugetlb.h>

struct carrefour_run_stats     run_stats;
struct carrefour_global_stats  global_stats;
struct carrefour_hook_stats_t  hook_stats;

static void decide_pages_fate(void); 

unsigned long global_maptu;
extern unsigned long sampling_rate;
extern unsigned long iteration_length;

unsigned enable_replication = 1;
unsigned enable_interleaving = 1;
unsigned enable_migration = 1;
unsigned enable_splitting = 1;
unsigned enable_hotpage_tracking = 1;

unsigned split_has_been_chosen = 0;
unsigned nr_thp_split = 0;

// The first time, to enable replication we need at least 100 orders
// because replicating a page is costly (creation of multiple pgd)
// Set to 1, if replication has already been enable once
// TODO: do it per PID
unsigned min_nr_orders_enable_replication = 500;
unsigned min_nr_samples_for_migration = 2; // Only active if interleaving is disabled. 1 otherwise.

int enable_carrefour;

unsigned long nr_accesses_node[MAX_NUMNODES];
unsigned long interleaving_distrib[MAX_NUMNODES];
unsigned long interleaving_distrib_max = 0;
unsigned long interleaving_proba[MAX_NUMNODES];
unsigned long nr_accesses_node_ibs[MAX_NUMNODES];

unsigned long current_accesses_to_node[MAX_NUMNODES];
unsigned long computed_accesses_to_node[MAX_NUMNODES];
unsigned long computed_accesses_to_node_4k[MAX_NUMNODES];

unsigned int it_no = 0;

void carrefour(void) {
   it_no++;
   decide_pages_fate();
}

/****
 * Algorithm plumbery for page migration / replication
 * Store pages in appropriate structs
 ****/


/* Each pid has a few struct pid_pages-> These structs basically store the pages accessed by a pid.
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
      if(p->nb_max_pages == MAX_PAGES_TO_WATCH_ACCURATE) {
         printk("[WARNING] How is it possible ?\n");
         return;
      }

      if(p->nb_max_pages) {
         p->nb_max_pages *= 2;
         if(p->nb_max_pages > MAX_PAGES_TO_WATCH_ACCURATE) {
            p->nb_max_pages = MAX_PAGES_TO_WATCH_ACCURATE;
         }
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
static inline int interleaving_random_node(int from_node, struct sdpage* this) {
   int dest_node = -1;
   int rand = random32()%101; /** Between 0 and 100% **/

   if(!enable_interleaving) {
      return -1;
   }

   if(! carrefour_module_options[REPLICATION_PER_TID].value) {
      if(rand <= interleaving_proba[from_node]) {
         rand = random32()%interleaving_distrib_max;
         for(dest_node = 0; interleaving_distrib[dest_node] < rand; dest_node++) {}

         if(dest_node >= num_online_nodes()) {
            printk("[WARNING] Bug ...\n");
            dest_node = -1;
         }
      }
   }
   else {
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
            dest_node = -1;
         }
      }
   }

   return dest_node;
}

static void decide_page_fate(struct sdpage *this, int alread_treated) {
   int nb_dies, dest_node;
   unsigned long total_nb_accesses;

   int should_be_interleaved = 0;
   int should_be_migrated = 0;
   int should_be_replicated = 0;

   int from_node;
   struct pages_container * interleave_container;

   dest_node = this->interleaving_chosen_node;
   total_nb_accesses = this->total_accesses_w_cache;
   nb_dies = this->nr_nodes;

   if(alread_treated) {
      run_stats.nr_of_samples_after_order   += total_nb_accesses;
      run_stats.nr_of_process_pages_touched ++;
   }

   from_node = phys2node((unsigned long) this->page_phys);

   if(unlikely(from_node >= num_online_nodes())) {
      // Just a sanity check
      BUG();
   }
      

   /*****
    * THIS IS THE REAL CORE AND IMPORTANT LOGIC
    * - duplicate pages accessed from multiple nodes in RO
    * - interleave pages accessed from multiple nodes in RW
    * - move pages accessed from 1 node
    ****/
   should_be_replicated = enable_replication && (nb_dies > 1) && (this->nb_writes == 0) && is_user_addr(this->page_lin) && !this->huge;
   should_be_replicated &= is_allowed_to_replicate(this->tgid);

   if(this->THP_page->split) {
      should_be_interleaved = (nb_dies > 1);
      should_be_migrated = (nb_dies == 1) && from_node != dest_node;
   }
   else {
      should_be_interleaved = enable_interleaving && (nb_dies > 1);

      should_be_migrated = nb_dies == 1;
      should_be_migrated = should_be_migrated && ((enable_migration && total_nb_accesses >= min_nr_samples_for_migration) || enable_interleaving) && from_node != dest_node;
   }

   interleave_container = &pages_to_interleave;
   
   /** The priority is to replicate if possible **/
   if(should_be_replicated) {
      //printk("Choosing replication for page %p:%d (total_nb_accesses = %lu, nb_dies = %d)\n", this->page_lin, this->tgid, total_nb_accesses, nb_dies);
      insert_page_in_container(&pages_to_replicate, this->tgid, this->page_lin, 0);
      run_stats.nr_requested_replications++;
   }
   else if (should_be_interleaved) {
      if(dest_node == -1) {
         dest_node = interleaving_random_node(from_node, this);
      }

      if(dest_node != -1 && (dest_node != from_node)) { 
         insert_page_in_container(interleave_container, this->tgid, this->page_lin, dest_node);
         run_stats.nb_interleave_orders++;
         //printk("Will interleave page on node %d\n", dest_node);

         run_stats.migr_from_to_node[from_node][dest_node]++;
      }
   }
   else if (should_be_migrated) { 
      //printk("Page %p will be migrated from node %d to node %d\n", this->page_lin, from_node, dest_node);
      insert_page_in_container(interleave_container, this->tgid, this->page_lin, dest_node);
      run_stats.nb_migration_orders++;
      run_stats.migr_from_to_node[from_node][dest_node]++;
   }
}

static inline void __get_page_max_current_la(struct sdpage *page, unsigned long total_accesses_ibs) {
   int j;
   int from_node = phys2node((unsigned long)(page->page_phys));

   unsigned * nb_accesses_ptr = page->nb_accesses;
   unsigned * nb_accesses_dram_ptr = page->nb_accesses_dram;

   unsigned total = 0, max = 0, nr_nodes = 0, dest_node = 0;
   unsigned total_dram = 0;

   unsigned percent_access_to_page;

   for(j = 0; j < num_online_nodes(); j++) {
      u64 accesses = nb_accesses_ptr[j];

      if(!accesses)
         continue;

      if(accesses > max) {
         max = accesses;
         dest_node = j;
      }

      nr_nodes++;
      total += nb_accesses_ptr[j];
   
      total_dram += nb_accesses_dram_ptr[j];
   }

   page->nr_nodes = nr_nodes;
   page->interleaving_chosen_node = dest_node;

   page->total_accesses = total;
   page->total_accesses_w_cache = total;
   page->local_accesses = nb_accesses_ptr[from_node];
   page->local_accesses_max = max;

   percent_access_to_page = total_accesses_ibs?(page->total_accesses * 100 / total_accesses_ibs):0;
   page->is_hot = (percent_access_to_page > 6LL);

   //if(page->accessed_by_multiple_threads && nnodes > 1) {
   if(page->nr_nodes > 1) {
      int node = interleaving_random_node(from_node, page);
      if(node == -1)
         node = from_node;

      page->local_accesses_max = nb_accesses_ptr[node];
      page->interleaving_chosen_node = node;
   }

   if(!total_dram) {
      page->local_accesses_max = 0;
      page->local_accesses = 0;
      page->total_accesses = 0;
   }
}

static int __array_stddev(unsigned long *array, int length) {
   unsigned long sum = 0;
   unsigned long mean;
   unsigned long stddev = 0;
   int i;

   for (i = 0; i < length; i++) {
      sum = sum + array[i];
   }

   mean = (length > 0) ? (sum / length) : 0;

   for (i = 0; i < length; i++) {
      unsigned long diff = array[i] - mean;
      stddev += diff * diff;
   }

   stddev /= length;

   stddev = int_sqrt(stddev);

   return (mean > 0) ? (stddev * 100 / mean) : 0;
}

void decide_pages_fate(void) {
   int i,j;
   struct pid_pages *p;
   int nr_regular_huge_pages, nr_thp, nr_misplaced_thp, nr_shared_thp, nr_thp_split_failed, nr_thp_migrate, nr_thp_migrate_failed;
   unsigned long total_accesses = 0, total_accesses_ibs = 0;

   struct rbtree_stats_t rbtree_stats;
   struct pagetree *pages_huge, *pages;

   u64 total_current = 0, total_current_w_cache = 0, total_4k = 0, total_local_current = 0, total_local_4k = 0, max_local_current = 0, max_local_4k = 0;

   u64 max_lar_4K = 0, max_lar_current = 0, current_lar = 0;
   int current_stddev = 0, estimated_stddev = 0, estimated_stddev_4k = 0;
   int lar_diff = 0, lar_diff_carrefour = 0, force_split = 0;

   unsigned hot_pages = 0;

   enum thp_states current_thp_state = get_thp_state();
   unsigned skip_lar_estimation = (!enable_splitting || (carrefour_module_options[OVER_PARANOID_NUMA_FIX].value && current_thp_state == THP_MADVISE)) && !carrefour_module_options[PROFILER_MODE].value;

   unsigned long start_split, stop_split, acc_split = 0;

   unsigned max_sample_page = 0, total_access_shared_page = 0;
   unsigned long max_percent_page;

   rbtree_get_merged_stats(&rbtree_stats, &run_stats);
   get_rbtree(&pages, &pages_huge);

   nr_regular_huge_pages = nr_thp = nr_misplaced_thp = nr_shared_thp = nr_thp_split = nr_thp_split_failed = nr_thp_migrate = nr_thp_migrate_failed = 0;

   /* Compute the imbalance and set up the right probabilities for interleaving */
   for(i = 0; i < num_online_nodes(); i++) { 
      //printk("Load on node %d : %lu memory accesses\n", i, nr_accesses_node[i]);
      total_accesses += nr_accesses_node[i];
      total_accesses_ibs += nr_accesses_node_ibs[i];
   }

   for(i = 0; i < num_online_nodes(); i++) {
      if(total_accesses) {
         interleaving_proba[i] = (nr_accesses_node[i]*100 / total_accesses);
         interleaving_distrib[i] = interleaving_distrib_max + (100 - interleaving_proba[i]);
         interleaving_distrib_max = interleaving_distrib[i];
      } else {
         interleaving_proba[i] = 100;
         interleaving_distrib[i] = interleaving_distrib_max + 100;
         interleaving_distrib_max = interleaving_distrib[i];
      }
   }

   if(!skip_lar_estimation) {
      memset(current_accesses_to_node, 0, sizeof(unsigned long) * num_online_nodes());
      memset(computed_accesses_to_node, 0, sizeof(unsigned long) * num_online_nodes());
      memset(computed_accesses_to_node_4k, 0, sizeof(unsigned long) * num_online_nodes());

      for(i = 0; i < pages_huge->reserve.index; i++) {
         struct sdpage * this = &pages_huge->reserve.pages[i];

         if(carrefour_module_options[KEEP_TRACK_SPLIT_PAGES].value) {
            this->split = has_been_split(((unsigned long) this->page_lin) & HGPAGE_MASK);
         }

         if(this->huge == -1) {
            this->huge = 0;
            continue;
         }

         if(! has_been_accessed_recently(this) || this->split || this->invalid) {
            continue;
         }

         if(!this->huge) {
            //int alread_treated;
            //this->invalid = page_status_for_carrefour(this->tgid, (unsigned long) this->page_lin, &alread_treated, &this->huge);
            int ret = is_huge_addr_sloppy(this->tgid, (unsigned long) this->page_lin);
            this->invalid = 0;
            this->huge = 0;

            if(unlikely(ret == -1)) {
               this->invalid = 1;
            }
            else if (ret == 1) {
               this->huge = 1;
            }

            if(this->invalid || !this->huge) {
               continue;
            }
         }
      }

      for(i = 0; i < pages->reserve.index; i++) {
         struct sdpage * this = &pages->reserve.pages[i];
         struct sdpage * corresponding_THP = this->THP_page;

         int from_node;
         
         if(!corresponding_THP) {
            if(unlikely(!pages_huge->warned_overflow))
               printk("[BUG, 0x%lx] Cannot find the associated huge page (0x%lx)\n", (unsigned long) this->page_phys, (unsigned long) corresponding_THP->page_phys);
            continue;
         }
         
         from_node = phys2node((unsigned long) corresponding_THP->page_phys);

         if(corresponding_THP->huge && !corresponding_THP->accounted) {
            __get_page_max_current_la(corresponding_THP, total_accesses_ibs);

            total_current += corresponding_THP->total_accesses;
            total_current_w_cache += corresponding_THP->total_accesses_w_cache;

            total_local_current += corresponding_THP->local_accesses;

#define BE_OVER_ACCURATE 0
#if BE_OVER_ACCURATE
            if(enable_carrefour) {
               max_local_current += corresponding_THP->local_accesses_max;
               computed_accesses_to_node[corresponding_THP->interleaving_chosen_node] += corresponding_THP->total_accesses;
            }
            else {
               max_local_current += corresponding_THP->local_accesses;
               computed_accesses_to_node[from_node] += corresponding_THP->total_accesses;
            }
#else
            max_local_current += corresponding_THP->local_accesses_max;
            computed_accesses_to_node[corresponding_THP->interleaving_chosen_node] += corresponding_THP->total_accesses;
#endif

            corresponding_THP->accounted = 1;
            current_accesses_to_node[from_node] += corresponding_THP->total_accesses;

            hot_pages += corresponding_THP->is_hot;

            if(corresponding_THP->total_accesses_w_cache > max_sample_page)
               max_sample_page = corresponding_THP->total_accesses_w_cache;


            if(corresponding_THP->nr_nodes > 1)
               total_access_shared_page += corresponding_THP->total_accesses_w_cache;
         }
         
         __get_page_max_current_la(this, total_accesses_ibs);

         total_4k += this->total_accesses;
         total_local_4k += this->local_accesses;

#if BE_OVER_ACCURATE
         if(enable_carrefour && corresponding_THP->huge && corresponding_THP->nr_nodes > 1) {
            // This page will be split so we might improve the LAR
            max_local_4k += this->local_accesses_max; 
            computed_accesses_to_node_4k[this->interleaving_chosen_node] += this->total_accesses;
         }
         else if (enable_carrefour && !corresponding_THP->huge) {
            // This is a 4k page, it's LAR can be improved by Carrefour
            max_local_4k += this->local_accesses_max; 
            computed_accesses_to_node_4k[this->interleaving_chosen_node] += this->total_accesses;
         }
         else {
            max_local_4k += this->local_accesses; 
            computed_accesses_to_node_4k[from_node] += this->total_accesses;
         }
#else
         max_local_4k += this->local_accesses_max; 
         computed_accesses_to_node_4k[this->interleaving_chosen_node] += this->total_accesses;
#endif

         if(!corresponding_THP->huge) {
            total_current += this->total_accesses;
            total_current_w_cache += this->total_accesses_w_cache;
            total_local_current += this->local_accesses;


#if BE_OVER_ACCURATE
            if(enable_carrefour) {
               max_local_current += this->local_accesses_max;
               computed_accesses_to_node[this->interleaving_chosen_node] += this->total_accesses;
            }
            else {
               max_local_current += this->local_accesses;
               computed_accesses_to_node[from_node] += this->total_accesses;
            }
#else
            max_local_current += this->local_accesses_max;
            computed_accesses_to_node[this->interleaving_chosen_node] += this->total_accesses;
#endif
        

            current_accesses_to_node[from_node] += this->total_accesses;
            
            hot_pages += this->is_hot;

            if(this->total_accesses_w_cache > max_sample_page)
               max_sample_page = this->total_accesses_w_cache;

            if(this->nr_nodes > 1)
               total_access_shared_page += this->total_accesses_w_cache;
         }
         
      }

      if(total_4k) {
         max_lar_4K = 100LL * max_local_4k / total_4k;
      }
      if(total_current) {
         max_lar_current = 100LL * max_local_current / total_current;
         current_lar = 100LL * total_local_current / total_current;
      }

      current_stddev = __array_stddev(current_accesses_to_node, num_online_nodes());
      estimated_stddev = __array_stddev(computed_accesses_to_node, num_online_nodes());
      estimated_stddev_4k = __array_stddev(computed_accesses_to_node_4k, num_online_nodes());


      // Note: for quite a lot of apps, lar2M > lar4K. That is NOT an abnormal behavior because lar2M is only computed on 2M pages
      lar_diff = ((int)max_lar_4K) - ((int)max_lar_current);
      lar_diff_carrefour = current_lar ? (max_lar_current * 100 / current_lar - 100) : 0;

      if(lar_diff >= carrefour_module_options[MIN_LAR_DIFF_TO_FORCESPLIT].value) {
         force_split = 1;
      }

      //if(enable_carrefour && lar_diff_carrefour >= 15) {
      if(lar_diff_carrefour >= 15) {
         // If we think that we can improve the LAR of 2M pages just by using the regular algorithm, do it
         force_split = 0;
      } 

      if(current_lar < 10) {
         // Something went very wrong with the estimation
         // Maybe no DRAM sample
         force_split = 0;
      }
   }

   if(!enable_splitting)
      force_split = 0;
   
   if(force_split && (++split_has_been_chosen < 2)) {
      force_split = 0;
   } 
   else if (!force_split) {
      split_has_been_chosen = 0;
   }

   if(enable_splitting && carrefour_module_options[PARANOID_NUMA_FIX].value) {
      if(force_split) {
         set_thp_state(THP_MADVISE);
      }
      else if((current_thp_state == THP_MADVISE) && carrefour_module_options[OVER_PARANOID_NUMA_FIX].value) {
         force_split = 1;
      }
   }

   if(force_split && !carrefour_module_options[KEEP_TRACK_SPLIT_PAGES].value) {
      enable_migration = 1;
   }

   printu("[%5d] GMAPTU = %lu LAR (Cur.: %d, Max.: %d, 4k: %d, Diff = %d, Diff carrefour = %d) Imbalance (Cur.: %d, Estimated = %d, 4k = %d) %u hot pages, enable_splitting=%d => sc = %d, force_split = %d\n", 
         it_no, global_maptu, (int) current_lar, (int)max_lar_current, (int)max_lar_4K, lar_diff, lar_diff_carrefour, current_stddev, estimated_stddev, estimated_stddev_4k, hot_pages, enable_splitting, split_has_been_chosen, force_split);

   
   max_percent_page = total_accesses_ibs ? (max_sample_page * 100 / total_accesses_ibs) : 0;

   printu("[%5d] %% shared pages = %llu %%, Nr hot pages = %u , %% access most accessed page = %lu %%\n", 
          it_no, total_current_w_cache? (total_access_shared_page * 100 / total_current_w_cache) : 0, hot_pages, max_percent_page);

   global_stats.total_access_shared_page += total_access_shared_page;
   global_stats.total_access += total_current_w_cache;

   if(max_percent_page > global_stats.max_percent_page) {
      global_stats.max_percent_page = max_percent_page;
   }

   if(hot_pages > global_stats.max_nr_hot_pages) {
      global_stats.max_nr_hot_pages = hot_pages;
   }
   

   for(i = 0; carrefour_module_options[CONSIDER_2M_PAGES].value && (i < pages_huge->reserve.index); i++) {
      struct sdpage * this = &pages_huge->reserve.pages[i];
      int nb_nodes = 0;
      int from_node = phys2node((unsigned long) this->page_phys);
     
      unsigned accessed_by_node = 0;
      
      rdtscll(start_split);
      if(unlikely(from_node >= num_online_nodes())) {
         // Just a sanity check
         BUG();
      }

      if(skip_lar_estimation) {
         if(carrefour_module_options[KEEP_TRACK_SPLIT_PAGES].value) {
            this->split = has_been_split(((unsigned long) this->page_lin) & HGPAGE_MASK);
         }

         if(this->huge == -1) {
            this->huge = 0;
            continue;
         }

         if(! has_been_accessed_recently(this) || this->invalid) {
            continue;
         }

         if(!this->huge) {
            //int alread_treated;
            //this->invalid = page_status_for_carrefour(this->tgid, (unsigned long) this->page_lin, &alread_treated, &this->huge);
            int ret = is_huge_addr_sloppy(this->tgid, (unsigned long) this->page_lin);
            this->invalid = 0;
            this->huge = 0;

            if(unlikely(ret == -1)) {
               this->invalid = 1;
            }
            else if (ret == 1) {
               this->huge = 1;
            }

            if(this->invalid || !this->huge) {
               continue;
            }
         }

         __get_page_max_current_la(this, total_accesses_ibs);
      }

      if(!has_been_accessed_recently(this) || this->invalid || !this->huge) {
         continue;
      }

      if(carrefour_module_options[PAGE_BOUNCING_FIX_2M].value && this->migrated && !force_split) {
         continue;
      }
      
      nr_thp++;

      /*if(!this->total_accesses)
         continue;*/

      nb_nodes = this->nr_nodes;
      accessed_by_node = this->interleaving_chosen_node;

      //if(nb_nodes > 1 && this->accessed_by_multiple_threads) {
      if(nb_nodes > 1) {
         nr_shared_thp++;

         if(force_split || (enable_hotpage_tracking && this->is_hot)) {
            int err = find_and_split_thp(this->tgid, (unsigned long) this->page_lin);

            if(this->is_hot)
               split_has_been_chosen = (split_has_been_chosen == 0) ? 1 : split_has_been_chosen;

            if(err) {
               printk("(%d) Split has failed (return value is %d)!\n", __LINE__, err);
               nr_thp_split_failed ++;
            }
            else {
               this->split = 1;

               if(carrefour_module_options[KEEP_TRACK_SPLIT_PAGES].value) {
                  register_new_split_page((unsigned long) this->page_lin);
               }

               this->huge = 0;
               nr_thp_split ++;
            }

            if(this->is_hot || carrefour_module_options[INTERLEAVE_AFTER_SPLIT].value) {
               // interleave the subpages (they now exist!)
               int kp, num_nodes_kp = num_online_nodes();
               for(kp = 0; kp < 512; kp++) {
                  insert_page_in_container(&pages_to_interleave, this->tgid, this->page_lin+(4096L*kp), kp%num_nodes_kp);
               }
            }

            rdtscll(stop_split);
            acc_split += (stop_split - start_split);
            if(carrefour_module_options[THROTTLE_SPLIT_RATE].value && acc_split *100 / iteration_length >= carrefour_module_options[THROTTLE_SPLIT_RATE].value) {
               force_split = 0;
            }
         }
         else if(enable_interleaving && carrefour_module_options[INTERLEAVE_SHARED_THP].value) {
            int dest_node = this->interleaving_chosen_node;

            //if(force_split || split_has_been_chosen || hot_pages)
            //   continue;

            if(dest_node == -1) {
               dest_node = interleaving_random_node(from_node, this);
            }

            if(dest_node != -1 && (dest_node != from_node)) {
               int err = find_and_migrate_thp(this->tgid, (unsigned long) this->page_lin, dest_node);
               switch(err) {
                  case 0:
                     nr_thp_migrate++;
                     this->migrated = 1;
                     break;
                  case -EBOUNCINGFIX:
                     this->migrated = 1;
                     break;
                  default:
                     printk("(%d) Migrate has failed (return value is %d)!\n", __LINE__, err);
                     nr_thp_migrate_failed ++;
               }
            }
         }
      }

      if((nb_nodes == 1  && accessed_by_node != from_node)) {
         nr_misplaced_thp++;

         if(force_split && unlikely(carrefour_module_options[SPLIT_MISPLACED_THP].value)) {
            int err = find_and_split_thp(this->tgid, (unsigned long) this->page_lin);

            if(err) {
               printk("(%d) Split has failed (return value is %d)!\n", __LINE__, err);
               nr_thp_split_failed ++;
            }
            else {
               if(this->is_hot || carrefour_module_options[INTERLEAVE_AFTER_SPLIT].value) {
                  // interleave the subpages (they now exist!)
                  int kp, num_nodes_kp = num_online_nodes();
                  for(kp = 0; kp < 512; kp++) {
                     insert_page_in_container(&pages_to_interleave, this->tgid, this->page_lin+(4096L*kp), kp%num_nodes_kp);
                  }
               }

               this->split = 1;
               this->huge = 0;
               nr_thp_split ++;
            }

            rdtscll(stop_split);
            acc_split += (stop_split - start_split);
            if(carrefour_module_options[THROTTLE_SPLIT_RATE].value && acc_split *100 / iteration_length >= carrefour_module_options[THROTTLE_SPLIT_RATE].value) {
               force_split = 0;
            }
         }
         else if (likely(carrefour_module_options[MIGRATE_MISPLACED_THP].value) && (enable_interleaving || enable_migration)) {
            int err;
            if(carrefour_module_options[PAGE_BOUNCING_FIX_2M].value && this->migrated) 
               continue;

            /*if(force_split || split_has_been_chosen || hot_pages)
               continue;*/

            err = find_and_migrate_thp(this->tgid, (unsigned long) this->page_lin, accessed_by_node);
            switch(err) {
               case 0:
                  nr_thp_migrate++;
                  this->migrated = 1;
                  break;
               case -EBOUNCINGFIX:
                  this->migrated = 1;
                  break;
               default:
                  printk("(%d) Migrate has failed (return value is %d)!\n", __LINE__, err);
                  nr_thp_migrate_failed ++;
            }
         }
      }
   }
   printu("%d regular huge pages, %d THP -- %d THP shared, %d misplaced -- %d THP split succeded, %d failed -- %d THP migrate succeded, %d failed\n", 
         nr_regular_huge_pages, nr_thp,
         nr_shared_thp, nr_misplaced_thp,
         nr_thp_split, nr_thp_split_failed,
         nr_thp_migrate, nr_thp_migrate_failed
         );

   for(i = 0; i < pages->reserve.index; i++) {
      int alread_treated = 0;
      struct sdpage * this = &pages->reserve.pages[i];

      int split = 0;

      if(skip_lar_estimation) {
         if(! has_been_accessed_recently(this) || this->invalid) {
            continue;
         }

         __get_page_max_current_la(this, total_accesses_ibs);
      }

      if(carrefour_module_options[DETAILED_STATS].value) {
         int huge;
         this->invalid = page_status_for_carrefour(this->tgid, (unsigned long) this->page_lin, &alread_treated, &huge);

         if(this->invalid) {
            continue;
         }
      }

      if(!this->THP_page) {
         if(unlikely(!pages_huge->warned_overflow))
            printk("[BUG, 0x%lx] Cannot find the associated huge page (0x%lx)\n", (unsigned long) this->page_phys, (unsigned long) this->THP_page->page_phys);
         continue;
      }

      if(this->THP_page->huge) {
         continue;
      }

      split = this->THP_page->split;
         
      if((carrefour_module_options[CONSIDER_4K_PAGES].value) || (carrefour_module_options[CONSIDER_2M_PAGES].value && split)) {
         //splitted and huge pages already treated before
         decide_page_fate(this, alread_treated);
      }
   }

   /* Migrate or interleave */
   for(p = pages_to_interleave.pids; p; p = p->next) {
      int err;

      //printk("Moving %d pages of pid %d!\n", p->nb_pages, p->tgid);
      err = s_migrate_pages(p->tgid, p->nb_pages, p->pages, p->nodes, 0);

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

   if(run_stats.nb_migration_orders + run_stats.nb_interleave_orders) {
      for(i = 0; i < num_online_nodes(); i++) {
         printk("Moving pages from node %d: ", i);
         for(j = 0; j < num_online_nodes(); j++) {
            printk("%d\t", run_stats.migr_from_to_node[i][j]);
         }
         printk("\n");
      }
   }

   /* replicate */
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
      }
   }

   run_stats.total_nr_orders = run_stats.nb_migration_orders + run_stats.nb_interleave_orders + run_stats.nb_replication_orders;

   global_stats.total_nr_orders += run_stats.total_nr_orders;

   /* Clean the mess */
   carrefour_free_pages(&pages_to_interleave);
   carrefour_free_pages(&pages_to_replicate);

   printu("Carrefour - %d migration %d interleaving %d replication orders\n",
         run_stats.nb_migration_orders, run_stats.nb_interleave_orders, run_stats.nb_replication_orders
         );

   printu("%lu pages, %lu samples, avg = %lu\n", 
         rbtree_stats.nr_pages_in_tree, rbtree_stats.total_samples_in_tree,
         (unsigned long) run_stats.avg_nr_samples_per_page);

   if(carrefour_module_options[DETAILED_STATS].value) {
      printu("NPPT = %lu -- NSAAO = %lu -- TNO = %lu -- TNSIT = %lu -- TNSM = %lu\n", 
            run_stats.nr_of_process_pages_touched, run_stats.nr_of_samples_after_order, global_stats.total_nr_orders, rbtree_stats.total_samples_in_tree, rbtree_stats.total_samples_missed);
   }
}


void carrefour_init(void) {
   memset(&run_stats, 0, sizeof(struct carrefour_run_stats));

   memset(&pages_to_interleave, 0, sizeof(pages_to_interleave));
   memset(&pages_to_replicate, 0, sizeof(pages_to_replicate));

   memset(nr_accesses_node, 0, sizeof(unsigned long) * num_online_nodes());
   memset(nr_accesses_node_ibs, 0, sizeof(unsigned long) * num_online_nodes());

   memset(interleaving_distrib, 0, sizeof(unsigned long) * num_online_nodes());
   memset(interleaving_proba, 0, sizeof(unsigned long) * num_online_nodes());
   
   interleaving_distrib_max = 0;
}

void carrefour_clean(void) {
}

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

#if ENABLE_THREAD_PLACEMENT
/** Tids we have seen **/
static int nb_seen_tids = 0;
static pid_t *seen_tids = NULL;
static int *tids_locality = NULL;
static int *common_pages = NULL;


/** Stats about tids we have seen **/
/** This should be integrated in struct task_struct (seen sched.h:1237) but I *don't* want to recompile the kernel **/
static struct tid_stat {
   u64 last_seen;
   u64 last_exec_time;
   pid_t tid;
   int last_chosen_node;
} *tid_stats = NULL;
static size_t tid_stats_size = 0, tid_stats_max_size = 0;


void tids_init(void) {
   /*tid_stats_size = 0;
   tid_stats_max_size = 0;
   tid_stats = NULL;*/

   nb_seen_tids = 0;
   seen_tids = kmalloc(sizeof(*seen_tids)*MAX_PIDS_TO_WATCH, GFP_KERNEL);
   if(!seen_tids) 
      panic("Cannot allocate seen_tids\n");
   memset(seen_tids, 0, sizeof(*seen_tids)*MAX_PIDS_TO_WATCH);
   tids_locality = kmalloc(sizeof(*tids_locality)*MAX_PIDS_TO_WATCH*num_online_nodes(), GFP_KERNEL);
   if(!tids_locality)
      panic("Cannot allocate tids_locality\n");
   memset(tids_locality, 0, sizeof(*tids_locality)*MAX_PIDS_TO_WATCH*num_online_nodes());
   common_pages = kmalloc(sizeof(*common_pages)*MAX_PIDS_TO_WATCH*MAX_PIDS_TO_WATCH, GFP_KERNEL);
   if(!common_pages)
      panic("Cannot allocate common_pages\n");
   memset(common_pages, 0, sizeof(*common_pages)*MAX_PIDS_TO_WATCH*MAX_PIDS_TO_WATCH);
}

void tids_clean(void) {
   if(common_pages)
	   kfree(common_pages);
   common_pages = NULL;
   if(seen_tids)
      kfree(seen_tids);
   seen_tids = NULL;
   nb_seen_tids = 0;
   if(tids_locality)
      kfree(tids_locality);
   tids_locality = NULL;
   /*if(tid_stats)
      kfree(tid_stats);
   tid_stats = NULL;
   tid_stats_size = 0;
   tid_stats_max_size = 0;*/
}

int tids_nb_seen_tids(void) {
   return nb_seen_tids;
}

int get_tid_index(pid_t tid) {
   int i;
   if(!seen_tids) {
	   WARN_ONCE(!seen_tids, "seen_tid has not been initialized (should have panic'ed)?!\n");
	   return MAX_PIDS_TO_WATCH + 1;
   }

   for(i = 0; i < MAX_PIDS_TO_WATCH; i++) {
      if(seen_tids[i] == tid)
         return i;
      if(seen_tids[i] == 0) {
         seen_tids[i] = tid;
         nb_seen_tids++;
         return i;
      }
   }
   return MAX_PIDS_TO_WATCH + 1;
}

pid_t get_tid(int index) {
   return seen_tids[index];
}

void increment_tid_node_access(pid_t tid_index, int node) {
   if(tid_index > MAX_PIDS_TO_WATCH || node >= num_online_nodes() || !tids_locality) {
      printk("Bug in increment_tid_node_access: tid %d, node %d, tids_locality %p?\n", tid_index, node, tids_locality);
      return;
   }
   tids_locality[num_online_nodes()*tid_index + node]++;
}

void increment_tid_counts(pid_t tid_index, u64 *bitmask) {
   int i;
   if(!common_pages) {
      WARN_ONCE(!common_pages, "common pages has not been allocated (should have panic'ed)?!\n");
      return;
   }
   if(tid_index > MAX_PIDS_TO_WATCH) {
      WARN_ONCE(tid_index > MAX_PIDS_TO_WATCH, "Strange tid_index: %d\n", tid_index);
      return;
   }

   for(i = 0; i < nb_seen_tids; i++) {
      if(IS_SET_TID(bitmask, i)) {
         common_pages[i + tid_index*MAX_PIDS_TO_WATCH]++;
      }
   }
}

void show_tid_sharing_map(void) {
   int nb_tids, i, j;
   nb_tids = tids_nb_seen_tids();

   printk("#Seen tids= %d\n", nb_tids);
   for(i = 0; i < nb_tids; i++) {
      int sharing = 0;
      int sharings_sum = 0;
      for(j = 0; j < nb_tids; j++) {
         printk("\ti->j = %d -> %d = %d\n", i, j, common_pages[i*MAX_PIDS_TO_WATCH + j]);
         if(j == i) 
            continue;
         if(common_pages[i*MAX_PIDS_TO_WATCH + j]) {
            sharing++;
            sharings_sum += common_pages[i*MAX_PIDS_TO_WATCH + j];
         }
      }
      printk(" #TID %d (%d): sharing with %d threads (%d sharing / %d accesses)\n", i, seen_tids[i], sharing, sharings_sum, common_pages[i*MAX_PIDS_TO_WATCH + i]);
   }
}

static int *_tids_compute_clusters(int cluster_thres) {
   int *tid_to_cluster = kmalloc(sizeof(*tid_to_cluster)*MAX_PIDS_TO_WATCH, GFP_KERNEL);
   int nb_tids, i, j;
   int nb_changes, nb_iterations = 0;
   nb_tids = tids_nb_seen_tids();

   for(i = 0; i < nb_tids; i++)
      tid_to_cluster[i] = i;

   do {
      nb_changes = 0;
      nb_iterations++;
      for(i = 0; i < nb_tids; i++) {
         for(j = 0; j < nb_tids; j++) {
            if(common_pages[i*MAX_PIDS_TO_WATCH + j] >= cluster_thres) {
               if(tid_to_cluster[j] < tid_to_cluster[i]) {
                  tid_to_cluster[i] = tid_to_cluster[j];
                  nb_changes++;
               } else if(tid_to_cluster[i] < tid_to_cluster[j]) {
                  tid_to_cluster[j] = tid_to_cluster[i];
                  nb_changes++;
               }
            }
         }
      }
   } while(nb_changes);

   /*printk("#Clustering %d iterations\n", nb_iterations);
   for(i = 0; i < nb_tids; i++) {
      printk(" #TID %d (%d): cluster %d\n", i, seen_tids[i], tid_to_cluster[i]);
   }*/

   return tid_to_cluster;
}
int *tids_compute_clusters(void) {
   return _tids_compute_clusters(MIN_ACCESS_TO_CLUSTER);
}






/***
 * Code that is not currently used.
 * It basically computes the "weight" of thread clusters (i.e., the amount of expected CPU time they can consume).
 * I don't think it that useful.
 */

/* Compute the weight of a given thread */
#define ONE_NS (1000000000L)
static u64 compute_tid_weight(struct task_struct *task, pid_t tid, u64 now) {
   size_t i;
   u64 process_duration;
   for(i = 0; i < tid_stats_size; i++) {
      if(tid_stats[i].tid == tid) {
         u64 weight = (now - tid_stats[i].last_seen)?100L*ONE_NS*(task->utime+task->stime-tid_stats[i].last_exec_time)/(now - tid_stats[i].last_seen):0;
	 //printk("Last seen %dms exec time %llu  %llu\n", (int)((now - tid_stats[i].last_seen)/1000000L), (task->utime+task->stime-tid_stats[i].last_exec_time), (unsigned long long)(task->utime+task->stime));
         tid_stats[i].last_seen = now;
         tid_stats[i].last_exec_time = task->utime+task->stime;
         return weight;
      }
   }

   if(tid_stats_size >= tid_stats_max_size) {
	   tid_stats_size = 0;
   }
   tid_stats[tid_stats_size].tid = tid;
   tid_stats[tid_stats_size].last_seen = now;
   tid_stats[tid_stats_size].last_exec_time = task->utime+task->stime;
   tid_stats_size++;

   process_duration = (now - (task->real_start_time.tv_sec*ONE_NS + task->real_start_time.tv_nsec));
   //printk("#First time seen pid %d launched for %ld weight %ld\n", tid, (long)process_duration, (long)(process_duration?(100L*ONE_NS*(task->utime+task->stime)/process_duration):0));
   return process_duration?(100L*ONE_NS*(task->utime+task->stime)/process_duration):0;
}

/* Compute the weight of a given cluster */
/* @clusters must have been preprocessed by simplify_clusters */
/* Dont' forget to free the return value */
u64* compute_clusters_weight(int nb_clusters, int *clusters) {
   struct task_struct *task;
   rwlock_t *lock = (rwlock_t*)(void*)tasklist_lock_hook;
   int nb_tids = tids_nb_seen_tids();
   u64 *clusters_weight = kmalloc(sizeof(*clusters_weight)*nb_clusters, GFP_KERNEL);
   int i;
   struct timespec now;
   u64 now64;

   do_posix_clock_monotonic_gettime(&now);
   now64 = now.tv_sec*ONE_NS + now.tv_nsec;


   memset(clusters_weight, 0, sizeof(*clusters_weight)*nb_clusters);

   if(tid_stats_max_size == 0) {
	   tid_stats_max_size = 128;
	   tid_stats_size = 0;
	   tid_stats = krealloc(tid_stats, tid_stats_max_size*sizeof(*tid_stats), GFP_KERNEL);
	   memset(tid_stats, 0, tid_stats_max_size*sizeof(*tid_stats));
   }

   for(i = 0; i < nb_tids; i++) {
      read_lock(lock);
      task = find_task_by_vpid_hook(get_tid(i));
      if(task) {
         if(clusters[i] > nb_clusters) {
            printk("compute_clusters_weight: clusters[%d] = %d > %d (use simplify_clusters on the cluster before calling this function\n",
                 i, clusters[i], nb_clusters); 
	    read_unlock(lock);
            continue;
         }
         clusters_weight[clusters[i]] += compute_tid_weight(task, get_tid(i), now64);
      }
      read_unlock(lock);
   }

   return clusters_weight;
}



/* Compute the "best" node for a cluster, based on previous memory accesses */
/* @clusters must have been preprocessed by simplify_clusters */
/* Dont' forget to free the return value */
int *compute_clusters_best_node(int nb_clusters, int *clusters) {
   int i, j;
   int nb_tids = tids_nb_seen_tids();
   int *clusters_nodes_accesses = kmalloc(sizeof(*clusters_nodes_accesses)*nb_clusters*num_online_nodes(), GFP_KERNEL);
   int *clusters_best_node = kmalloc(sizeof(*clusters_best_node)*nb_clusters, GFP_KERNEL);

   memset(clusters_nodes_accesses, 0, sizeof(*clusters_nodes_accesses)*nb_clusters*num_online_nodes());
   for(i = 0; i < nb_tids; i++) {
      if(clusters[i] > nb_clusters) {
         printk("compute_clusters_best_node: clusters[%d] = %d > %d (use simplify_clusters on the cluster before calling this function\n",
               i, clusters[i], nb_clusters); 
         continue;
      }
      for(j = 0; j < num_online_nodes(); j++) {
         clusters_nodes_accesses[clusters[i]*num_online_nodes() + j] += tids_locality[i*num_online_nodes() + j];
      }
   }

   for(i = 0; i < nb_clusters; i++) {
      int best = 0;
      int best_value = clusters_nodes_accesses[i*num_online_nodes() + 0];
      /*for(j = 0; j < num_online_nodes(); j++) {
	      printk("Cluster %d node %d = %d\n", i, j, clusters_nodes_accesses[i*num_online_nodes() +j]);
      }*/
      for(j = 1; j < num_online_nodes(); j++) {
         if(clusters_nodes_accesses[i*num_online_nodes() + j] > best_value) {
            best = j;
            best_value = clusters_nodes_accesses[i*num_online_nodes() + j];
         }
      }
      clusters_best_node[i] = best;
   }

   kfree(clusters_nodes_accesses);
   return clusters_best_node;
}



int simplify_clusters(int *clusters) {
   int nb_clusters = 0;
   int nb_tids = tids_nb_seen_tids();
   int *cluster_indexes;
   int i, j;
   for(i = 0; i < nb_tids; i++) {
      if(clusters[i] == i)
         nb_clusters++;
   }

   cluster_indexes = kmalloc(sizeof(*cluster_indexes)*nb_clusters, GFP_KERNEL);
   for(j = 0; j < nb_clusters; j++)
      cluster_indexes[j] = (nb_tids + 1);

   for(i = 0; i < nb_tids; i++) {
      for(j = 0; j < nb_clusters; j++) {
         if(cluster_indexes[j] == (nb_tids + 1)) {
            cluster_indexes[j] = clusters[i];
         }
         if(clusters[i] == cluster_indexes[j]) {
            clusters[i] = j;
            break;
         }
      }
   }

   kfree(cluster_indexes);

   return nb_clusters; 
}

#endif

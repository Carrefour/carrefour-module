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

#ifndef ibs_RBTREE
#define ibs_RBTREE 

#define MAX_PAGES_TO_WATCH_ACCURATE 30000
#define MAX_PAGES_TO_WATCH_CHEAP    10000

struct sdpage {
   struct rb_node node;
   void *page_phys,*page_lin;
   unsigned nb_accesses[MAX_NUMNODES];
   unsigned nb_accesses_dram[MAX_NUMNODES];
   pid_t tgid;
   int nb_writes;

   int huge;
   int split;
   int migrated;
   int invalid;

   int logical_time;

   int last_tid;
   int accessed_by_multiple_threads;

   // Computed stats
   int interleaving_chosen_node;
   int accounted;

   unsigned total_accesses;
   unsigned total_accesses_w_cache;
   unsigned local_accesses;
   unsigned local_accesses_max;
   unsigned nr_nodes;

   unsigned is_hot;

   struct sdpage * THP_page;
};

struct rbtree_stats_t {
   unsigned long  nr_ld_samples;
   unsigned long  nr_st_samples;
   unsigned long  total_samples_in_tree;
   unsigned long  total_samples_missed;
   unsigned long  nr_pages_in_tree;
};
 
struct page_reserve {
   unsigned index;
   unsigned max_pages_to_watch;
   struct sdpage* pages;
};

struct pagetree {
   struct rb_root       root;
   spinlock_t           lock;
   struct page_reserve  reserve;

   const char *         name;
   int                  warned_overflow;
   int                  initialized;
   int                  logical_time;
};

void rbtree_load_module(void);
void rbtree_remove_module(void);

void rbtree_init(void);
void rbtree_add_sample(int is_kernel, struct ibs_op_sample *ibs_op, int cpu, int pid, int tgid);
void rbtree_print(struct pagetree * tree); 
void rbtree_clean(void);
void rbtree_get_merged_stats(struct rbtree_stats_t * stats_to_fill, struct carrefour_run_stats * c_stats);
void get_rbtree(struct pagetree ** tree, struct pagetree ** tree_huge);

struct sdpage * insert_in_page_rbtree(struct rb_root *root, struct sdpage *data, int add);

int has_been_accessed_recently(struct sdpage * page);
void register_new_split_page(unsigned long page_phys);
int has_been_split(unsigned long page_phys);
 
#endif


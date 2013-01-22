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
#define MAX_PIDS_TO_WATCH 64 /* must be a multiple of 64 */

#define SET_TID(bitmask, index) \
   bitmask[(index)/64] = bitmask[(index)/64] | (1 << ((index)%64))
#define IS_SET_TID(bitmask, index) \
   bitmask[(index)/64] & (1 << ((index)%64))

struct sdpage {
	  	struct rb_node node;
	  	void *page_phys,*page_lin;
      int nb_accesses[MAX_NUMNODES];
      pid_t tgid;
      u64 tids[MAX_PIDS_TO_WATCH/64];
      int nb_writes;
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
   struct sdpage pages[MAX_PAGES_TO_WATCH_ACCURATE];
};

extern struct rb_root pagetree;
extern spinlock_t pagetree_lock;
extern struct page_reserve pagesreserve;

void rbtree_init(void);
void rbtree_add_sample(int is_kernel, struct ibs_op_sample *ibs_op, int cpu, int pid, int tgid);
void rbtree_print(void);
void rbtree_clean(void);
void rbtree_get_merged_stats(struct rbtree_stats_t * stats_to_fill, struct carrefour_run_stats * c_stats);

#endif


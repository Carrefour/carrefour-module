/*
Copyright (c) 
Linux kernel authors
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
#include <linux/ksm.h>

/** Taken from the kernel, move a list of pages   **/
/** Works on kernel memory instead of user memory **/
enum zone_type policy_zone = 0;
static inline int page_is_file_cache(struct page *page) 
{
   return !PageSwapBacked(page);
}

#ifndef FOLL_SPLIT
#define FOLL_SPLIT 0x80
#endif
extern struct carrefour_run_stats run_stats;

#if DUMP_OVERHEAD
u64 time_spent_in_migration;
#endif

static int do_move_page_to_node_array(struct mm_struct *mm,
				      struct page_to_node *pm,
				      int migrate_all)
{
	int err;
	struct page_to_node *pp;
	LIST_HEAD(pagelist);

	down_read(&mm->mmap_sem);
	/*
	 * Build a list of pages to migrate
	 */
	for (pp = pm; pp->node != MAX_NUMNODES; pp++) {
		struct vm_area_struct *vma;
		struct page *page;
      int current_node;

		err = -EFAULT;
		vma = find_vma(mm, pp->addr);
		if (!vma || pp->addr < vma->vm_start || !vma_migratable(vma))
			goto set_status;

		page = follow_page_hook(vma, pp->addr, FOLL_GET|FOLL_SPLIT);

		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto set_status;

		err = -ENOENT;
		if (!page)
			goto set_status;

		/* Use PageReserved to check for zero page */
		if (PageReserved(page) || PageKsm(page))
			goto put_and_set;

#if ENABLE_REPLICATION
      if(PageReplication(page)) {
         goto put_and_set;
      }
#endif

#if PAGE_BOUNCING_FIX
      if(page->stats.nr_migrations >= PAGE_BOUNCING_FIX) { // Page has been migrated already once
         goto put_and_set; 
      }
#endif

		pp->page = page;
		err = page_to_nid(page);

		if (err == pp->node)
			/*
			 * Node already in the right place
			 */
			goto put_and_set;

      current_node = err;

		err = -EACCES;
		if (page_mapcount(page) > 1 &&
				!migrate_all)
			goto put_and_set;

		err = isolate_lru_page_hook(page);
		if (!err) {
			list_add_tail(&page->lru, &pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					    page_is_file_cache(page));

         run_stats.migr_from_to_node[current_node][pp->node]++;
         run_stats.real_nb_migrations++;
		}
put_and_set:
		/*
		 * Either remove the duplicate refcount from
		 * isolate_lru_page() or drop the page ref if it was
		 * not isolated.
		 */
		put_page(page);
set_status:
		pp->status = err;
	}

	err = 0;
	if (!list_empty(&pagelist)) {
		err = migrate_pages_hook(&pagelist, new_page_node_hook,
				(unsigned long)pm, 0, true);
		/** it seems to oops so forget that **/
		if (err) {
			putback_lru_pages_hook(&pagelist);
		}
	}

   up_read(&mm->mmap_sem);

	return err;
}



static int do_pages_move(struct mm_struct *mm, struct task_struct *task,
      unsigned long nr_pages,
      const void ** pages,
      const int *nodes,
      int *status, int flags) {
   struct page_to_node *pm;
   unsigned long chunk_nr_pages;
   unsigned long chunk_start;
   int err;

   err = -ENOMEM;
   pm = (struct page_to_node *)__get_free_page(GFP_KERNEL);
   if (!pm)
      goto out;

   //lru_add_drain_all();
   lru_add_drain_all_hook();

   /*
    * Store a chunk of page_to_node array in a page,
    * but keep the last one as a marker
    */
   chunk_nr_pages = (PAGE_SIZE / sizeof(struct page_to_node)) - 1; 
   for (chunk_start = 0; 
        chunk_start < nr_pages;
        chunk_start += chunk_nr_pages) {
      int j;

      if (chunk_start + chunk_nr_pages > nr_pages)
         chunk_nr_pages = nr_pages - chunk_start;

      /* fill the chunk pm with addrs and nodes from user-space */
      for (j = 0; j < chunk_nr_pages; j++) {
         const void *p; //user
         int node;

         p = pages[j + chunk_start];
         pm[j].addr = (unsigned long) p;

         node = nodes[j + chunk_start];

         err = -ENODEV;
         if (node < 0 || node >= MAX_NUMNODES)
            goto out_pm;

         if (!node_state(node, N_HIGH_MEMORY))
            goto out_pm;

         pm[j].node = node;
      }    

      /* End marker for this chunk */
      pm[chunk_nr_pages].node = MAX_NUMNODES;

      /* Migrate this chunk */
      err = do_move_page_to_node_array(mm, pm, flags & MPOL_MF_MOVE_ALL);
      if (err < 0)
         goto out_pm;

      /* Return status information */
      if(status) 
         for (j = 0; j < chunk_nr_pages; j++)
            status[j + chunk_start] = pm[j].status;
   }
   err = 0;

out_pm:
   free_page((unsigned long)pm);
out:
   return err;
}

/** Actual ""syscall"" that must be called to migrate pages **/
int s_migrate_pages(pid_t pid, unsigned long nr_pages, void ** pages, int * nodes, int * status, int flags) {
   struct task_struct *task;
   struct mm_struct *mm;
   rwlock_t *lock = (rwlock_t*)(void*)tasklist_lock_hook;
   int err;
#if DUMP_OVERHEAD
   u64 end; 
   rdtscll(time_spent_in_migration);
#endif

   read_lock(lock);
   task = find_task_by_vpid_hook(pid);
   if(!task) {
      read_unlock(lock);
      return -ESRCH;
   }
   mm = get_task_mm(task);
   read_unlock(lock);
   if (!mm)
      return -EINVAL;

   err = do_pages_move(mm, task, nr_pages, (const void **)pages, (const int *)nodes, status, flags);

   mmput(mm);

#if DUMP_OVERHEAD
   rdtscll(end);
   time_spent_in_migration = (end - time_spent_in_migration);
#endif
   return err;
}


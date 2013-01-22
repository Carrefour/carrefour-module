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

#ifndef IBS_TIDS_H
#define IBS_TIDS_H

#if ENABLE_THREAD_PLACEMENT

// If threads do more than <this value> accesses on the same pages, then cluster them.
#define MIN_ACCESS_TO_CLUSTER 5

void tids_init(void);
void tids_clean(void);
int* tids_compute_clusters(void);  				/* Return an array [0..#seen tids] when array[i] = cluster of thread #i */
								//WARNING: kfree the result after using it!
int simplify_clusters(int *clusters);				/* Make sure that cluster are numbered 0..#clusters (apply this function on tids_compute_clusters result) */

/* Internal, used by ibs_rbtree.c and ibs_carrefour.c */
int tids_nb_seen_tids(void);					/* nb tids seen during profiling */
int get_tid_index(pid_t tid);					/* tid -> index [0..nb seen tids] */
pid_t get_tid(int index);					/* oposite function */
void increment_tid_counts(pid_t tid_index, u64 *bitmask);	/* called each time tid 'tid_index' does a memory access; bitmask = threads sharing pages with tid_index */
void show_tid_sharing_map(void);				/* debug function to show the matrix of interactions */
u64* compute_clusters_weight(int nb_clusters, int *clusters);   /* Give a weight to each cluster = CPU time is may consumme */
int *compute_clusters_best_node(int nb_clusters, int *clusters); /* Return an array[i] = best node for cluster i */
void increment_tid_node_access(pid_t tid_index, int node);

#endif

#endif

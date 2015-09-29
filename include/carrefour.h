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

#ifndef ibs_CARREFOUR
#define ibs_CARREFOUR

#include "carrefour_main.h"

void carrefour_init(void);
void carrefour_clean(void);
void carrefour(void);

struct carrefour_global_stats {
   unsigned long total_access_shared_page;
   unsigned long total_access;
   unsigned long max_percent_page;
   unsigned long max_nr_hot_pages;

   unsigned long total_nr_orders;
};

struct carrefour_run_stats {
   unsigned nb_migration_orders;
   unsigned nb_interleave_orders;
   unsigned nb_replication_orders;

   double avg_nr_samples_per_page;

   unsigned long nr_of_samples_after_order;
   unsigned long nr_of_process_pages_touched;

   unsigned long total_nr_orders;
   unsigned nr_requested_replications;
   unsigned migr_from_to_node[MAX_NUMNODES][MAX_NUMNODES];

   u64 time_start_profiling;
   u64 time_spent_in_migration;
   u64 time_spent_in_split;
   u64 time_spent_in_NMI;
};

extern struct carrefour_run_stats run_stats;
#endif

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

#if DETAILED_STATS
int page_has_already_been_treated(int pid, unsigned long addr);
#endif

struct carrefour_global_stats {
   unsigned cumulative_real_nb_migrations;
   unsigned cumulative_nb_migration_orders;
   unsigned cumulative_nb_interleave_orders;
   unsigned cumulative_nb_replication_orders;

   unsigned long total_nr_orders;
};

struct carrefour_run_stats {
   unsigned real_nb_migrations;
   unsigned nb_migration_orders;
   unsigned nb_interleave_orders;
   unsigned nb_replication_orders;

   double avg_nr_samples_per_page;
#if PREDICT_WITH_STDDEV
   double stddev_nr_samples_per_page;
#endif

#if DETAILED_STATS
   unsigned long nr_of_samples_after_order;
   unsigned long nr_of_process_pages_touched;
#endif

   unsigned long total_nr_orders;

   unsigned migr_from_to_node[MAX_NUMNODES][MAX_NUMNODES];
   unsigned nr_requested_replications;

#if ENABLE_THREAD_PLACEMENT
   unsigned nb_thr_migration;
   unsigned nb_failed_thr_migration;
   unsigned nb_useless_thr_migration;
#endif
};


#endif

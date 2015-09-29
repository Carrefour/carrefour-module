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

#ifndef __CARREFOUR_OPTIONS__
#define __CARREFOUR_OPTIONS__

enum carrefour_available_options {
   // General purpose
   ENABLE_REPLICATION = 0,
   ENABLE_MIGRATION,
   ENABLE_INTERLEAVING,
   DETAILED_STATS,

   // Related to IBS
   ADAPTIVE_SAMPLING, 
   IBS_RATE_ACCURATE,
   IBS_RATE_CHEAP,
   IBS_RATE_NO_ADAPTIVE,
   IBS_INSTRUCTION_BASED,
   IBS_CONSIDER_CACHES,
   IBS_ADAPTIVE_MAGIC,

   // Related to migration
   PAGE_BOUNCING_FIX_4K,
   PAGE_BOUNCING_FIX_2M,

   // Related to replication
   REPLICATION_PER_TID,

   // Related to huge pages
   SPLIT_SHARED_THP,
   INTERLEAVE_SHARED_THP,

   SPLIT_MISPLACED_THP,
   MIGRATE_MISPLACED_THP,

   ENABLE_HOT_PAGE_TRACKING,
   INTERLEAVE_AFTER_SPLIT,

   MIN_LAR_DIFF_TO_FORCESPLIT,

   PARANOID_NUMA_FIX,
   OVER_PARANOID_NUMA_FIX,

   KEEP_TRACK_SPLIT_PAGES,

   THROTTLE_SPLIT_RATE,

   // OTHERS
   CONSIDER_2M_PAGES,
   CONSIDER_4K_PAGES,
   
   PROFILER_MODE,

   // Dummy value
   CARREFOUR_OPTIONS_MAX,
};

struct carrefour_module_option_t {
   char *   description;
   int      value;
};

extern const struct carrefour_module_option_t carrefour_module_options[CARREFOUR_OPTIONS_MAX];

void print_module_options (void);
int validate_module_options (void);

#endif

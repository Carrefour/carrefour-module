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

#define MAX_TIDS_TO_REPLICATE 3000 /* big number, don't care */

struct rb_root replicationtidtree;

struct sdtid {
   struct rb_node node;
   int tgid;
   int replication_allowed;
};

struct tid_reserve {
   int index;
   struct sdtid tids[MAX_TIDS_TO_REPLICATE];
};
static struct tid_reserve tidsreserve;


/** init **/
void replicationtid_init(void) {
   replicationtidtree = RB_ROOT;
   tidsreserve.index = 0;
}

/** rbtree insert; for some reason the kernel does not provide an implem... */
static struct sdtid * insert_in_page_tidrbtree(struct rb_root *root, struct sdtid *data, int add) {
   struct rb_node **new = &(root->rb_node), *parent = NULL;

   /* Figure out where to put new node */
   while (*new) {
      struct sdtid *this = container_of(*new, struct sdtid, node);
      parent = *new;
      if (data->tgid > this->tgid)
         new = &((*new)->rb_left);
      else if (data->tgid < this->tgid)
         new = &((*new)->rb_right);
      else
         return this;
   }

   /* Add new node and rebalance tree. */
   if(add) {
      rb_link_node(&data->node, parent, new);
      rb_insert_color(&data->node, root);
      return data;
   } else {
      return NULL;
   }
}

void change_replication_state(int pid, int allow) {
   struct sdtid *tmp, *tmp2;
   if(tidsreserve.index >= MAX_TIDS_TO_REPLICATE)
      return;

   tmp = &tidsreserve.tids[tidsreserve.index];
   tmp->tgid = pid;

   tmp2 = insert_in_page_tidrbtree(&replicationtidtree, tmp, 1);
   if(tmp2 == tmp)
      tidsreserve.index++;

   tmp2->replication_allowed = allow;
}

int is_allowed_to_replicate(int pid) {
   struct sdtid tmp, *tmp2;

   if(!carrefour_module_options[REPLICATION_PER_TID].value) {
      return 1;
   }

   tmp.tgid = pid;

   tmp2 = insert_in_page_tidrbtree(&replicationtidtree, &tmp, 0);
   if(!tmp2)
      return 0; //default is "do not replicate"

   return tmp2->replication_allowed;
}

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

int get_nb_cores_per_node(void) {
	return nr_cpus_node(0);
}

static u64 nodes_phys_end[MAX_NUMNODES];
int phys2node(u64 physaddr) {
   int j;
   for(j = 0; j < num_online_nodes(); j++) { 
      if(physaddr < nodes_phys_end[j])
         return j;
   }
   return num_online_nodes() + 1;
}

unsigned long node2physend(int node) {
   return nodes_phys_end[node];
}

int random_cpu_of(int node) {
   const struct cpumask *c = cpumask_of_node(node);
   return cpumask_any(c);
}

/** Link Mask notation:
 *  - one link in the system is described as 0xXY X = node, Y = link (Y=0 = link to DRAM)
 *  - a mask is a combination of links; eg.: 0xXYZW = link XY and ZW
 */
static unsigned long linkmasks[MAX_NUMNODES][MAX_NUMNODES];

/* Low level plumbery */
typedef unsigned device_t;
struct route {
   unsigned int RQRte:9;
   unsigned int RPRte:9;
   unsigned int BCRte:9;
   unsigned int reserved:5;
};
static unsigned int pci_read_config32(device_t dev, unsigned where)
{
   unsigned addr;
   addr = dev | where;
   outl(0x80000000 | (addr & ~3), 0xCF8);
   return inl(0xCFC);
}
static void read_ht_conf(int from, int to) {
   unsigned int t;
   unsigned int link;
   struct route *r = (struct route *)&t;
   struct link l;

   if(to != from) {
      l.node = to; //we use the destination node link
      l.link = 0xF;
      linkmasks[from][to] <<= 8;
      linkmasks[from][to] |= l.val;
   }

   t = pci_read_config32((0<<16)+((24+from)<<11)+(0<<8), 0x40+(0x4*to));
   link = r->RQRte;
   if(!link)
      return;

   /* We use a link */
   l.node = from;

   if(link & 0x1)
      l.link = 0xF; //self
   if(link & 0x2)
      l.link = 0x1; //link0
   if(link & 0x4)
      l.link = 0x2; //link1
   if(link & 0x8)
      l.link = 0x3; //link2
   if(link & 0x10)
      l.link = 0x4; //link3
   if(link & 0x20)
      l.link = 0x5;
   if(link & 0x40)
      l.link = 0x6;
   if(link & 0x80)
      l.link = 0x7;
   if(link & 0x100)
      l.link = 0x8;

   linkmasks[from][to] <<= 8;
   linkmasks[from][to] |= l.val;
}

static void read_ht_conf_1hop(int from, int to) {
   unsigned int t, i;
   unsigned int link;
   struct route *r = (struct route *)&t;
   struct link l;

   if(from == to)
      return;

   t = pci_read_config32((0<<16)+((24+from)<<11)+(0<<8), 0x40+(0x4*to));

   /* If we bcast things coming from "to" then update the "to" linkmask */
   link = r->BCRte;
   if(!link || link == 0x1) // only self BCast
      return;

   /* We make the assumption that only 1 core uses us as forwarder. */
   l.node = from; //to uses our link to forward msg
   if(link & 0x2)
      l.link = 0x1;
   if(link & 0x4)
      l.link = 0x2;
   if(link & 0x8)
      l.link = 0x3;
   if(link & 0x10)
      l.link = 0x4;
   if(link & 0x20)
      l.link = 0x5;
   if(link & 0x40)
      l.link = 0x6;
   if(link & 0x80)
      l.link = 0x7;
   if(link & 0x100)
      l.link = 0x8;
   
   for(i = 0; i < num_online_nodes(); i++) {
      int mlink = linkmasks[from][i];
      while(mlink & 0xFF) {
         if((mlink & 0xFF) == l.val) {
            linkmasks[to][i] <<= 8;
            linkmasks[to][i] |= l.val;
         }
         mlink >>= 8;
      }
   }
   printk("Linkmask from %d to %d %lx\n", from, to, linkmasks[from][to]);
}
static struct link linkindexes[IBS_MAX_NB_LINKS];
static void simplify_linkmasks(void) {
   int i, j, k;
   unsigned long mask, link;
   for(i = 0; i < num_online_nodes(); i++) {
      for(j = 0; j < num_online_nodes(); j++) {
         link = linkmasks[i][j];
         mask = 0;
         while(link) {
            k = 0;
            while(k < IBS_MAX_NB_LINKS && linkindexes[k].val != 0) {
               if(linkindexes[k].val == (link & 0xFF)) {
                  mask |= (1<<k);
                  break;
               }
               k++;
            }
            if(k < IBS_MAX_NB_LINKS && linkindexes[k].val == 0) {
                  mask |= (1<<k);
                  linkindexes[k].val = (link & 0xFF);
            }
            link >>=8;
         }
         linkmasks[i][j] = mask;
      }
   }
}
struct link linkindex2link(int index) {
   if(index >= IBS_MAX_NB_LINKS || index < 0) {
      printk("ERROR: linkindex2link index=%d\n", index);
      return linkindexes[0];
   }
   return linkindexes[index];
}
char *linkindex2sentence(int index, char *buffer, int size) {
   struct link l = linkindex2link(index);
   int i,j;
   int n = snprintf(buffer, size, "Link %d is link %d of Node %d ", index, l.link, l.node);
   size -= n;
   for(i = 0; i < num_online_nodes(); i++) {
	if(linkmasks[l.node][i] & (1<<index)) {
		int nb_non_null = 0;
		for(j = 0; j < IBS_MAX_NB_LINKS; j++) {
			if(linkmasks[l.node][i] & (1<<j)) {
				nb_non_null++;
			}
		}
		if(nb_non_null == 2) {
			snprintf(buffer + n, size, "going to Node %d", i);
			break;
		} else if(nb_non_null == 1) {
			snprintf(buffer + n, size, "going to Node %d (self)", i);
			break;
		}
	}
   }
   return buffer;
}

unsigned long access2usedlink(int from, int to) {
   return linkmasks[from][to];
}

void machine_init(void) {
   int i, j;
   for(i = 0; i < IBS_MAX_NB_LINKS; i++) 
      linkindexes[i].val = 0;
   for(i = 0; i < num_online_nodes(); i++) 
      for(j = 0; j < num_online_nodes(); j++) 
         linkmasks[i][j] = 0;
   for(i = 0; i < num_online_nodes(); i++) 
      for(j = 0; j < num_online_nodes(); j++) 
         read_ht_conf(i, j);
   for(i = 0; i < num_online_nodes(); i++) 
      for(j = 0; j < num_online_nodes(); j++) 
         read_ht_conf_1hop(i, j);
   simplify_linkmasks();



   for(j = 0; j < num_online_nodes(); j++) { 
	   if(!NODE_DATA(j)) 
		   continue; 
	   nodes_phys_end[j] =  node_end_pfn(j)*(4LL*1024LL); 
   }




   /*for(i = 0; i < IBS_NB_NODES; i++) {
        for(j = 0; j < IBS_NB_NODES; j++) {
           struct link l;
           unsigned int link = linkmasks[i][j];
           int k;
           for(k = 0; k < IBS_MAX_NB_LINKS; k++) {
              if(link & (1<<k)) {
                 l = linkindex2link(k);
                 printk("From %d to %d: link %x (0xf=local) of node %x = link #%d\n", i, j, l.link, l.node, k);
              }
           }
        }
     }*/
}

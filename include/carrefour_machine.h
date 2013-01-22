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

#ifndef __CARREFOUR_MACHINE__
#define __CARREFOUR_MACHINE__

void machine_init(void);
int phys2node(u64 physaddr);           // Return the node that 'contains' physical address physaddr
unsigned long node2physend(int node); 
int random_cpu_of(int node);           // Return a random CPU of a given node

/** Topology of the machine **/
#define IBS_MAX_CORE     48
#define IBS_MAX_NB_LINKS 16    // There is at most 16 links in a 4 socket machine (4 per socket)
//#define IBS_MAX_NB_LINKS 32      // ... 32 links ... 8 socket
struct link {
   union {
      struct {
         unsigned int link:4; // link number 1-4 = HT links ; 0xf = link to the memory bank
         unsigned int node:4; // the link is on node x
      };
      struct {
         unsigned int val:8; // hacky stuff to ease comparison between link structs...
      };
   };
};

unsigned long access2usedlink(int from, int to); //Return a bitmask of used links 1001b = uses link 0 and 3
struct link linkindex2link(int index);           //Return the struct link corresponding to a link index (e.g. linkindex2link(0) and (3) to know what the previous bitmask represents
char *linkindex2sentence(int index, char *buffer, int size); //Pretty print what is link 'index'. Store the sentence in the buffer.
int get_nb_cores_per_node(void);

#endif

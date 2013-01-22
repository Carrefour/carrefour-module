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
#include "fake_ibs.h"
#include <linux/kthread.h>

#if FAKE_IBS

static unsigned long nr_samples_generated;
struct task_struct * work_thread;

unsigned long min_lin_address = MIN_LIN_ADDR;
unsigned long max_lin_address = MAX_LIN_ADDR;

unsigned sampling_rate_accurate  = 1;
unsigned sampling_rate_cheap     = 1;
unsigned sampling_rate           = 1;

int consider_L1L2 = 0;

#if FAKE_IBS
static unsigned long pids[MAX_PIDS_PER_APP];
static unsigned long npids = 0;
#endif 


static int handle_ibs_fake(void* arg) {
   int ndies = num_online_nodes();
   struct task_struct * task;
   char comm[TASK_COMM_LEN];
   unsigned int pid = 0;
   unsigned int tgid = 0;

#ifdef APP_TO_CONSIDER
   int found = 0;
   memset(comm, 0, TASK_COMM_LEN);

   rcu_read_lock();
   for_each_process(task){
      get_task_comm(comm, task);

      if(! strcmp(comm, APP_TO_CONSIDER) ) {
         struct task_struct *p;
         struct pid *pgrp;
         
         pid = task_pid_vnr(task);
         pgrp = find_get_pid(pid);

         //printk("Master = %d\n", pid);
         pids[npids++] = pid;

         do_each_pid_thread(pgrp, PIDTYPE_PGID, p) {
            pids[npids++] = task_pid_vnr(p);
            //printk("Child : %d\n", task_pid_vnr(p)); 
            if(npids == MAX_PIDS_PER_APP) {
               break;
            }
         } while_each_pid_thread(pgrp, PIDTYPE_PGID, p);

         tgid = task_tgid_nr_ns(task, ns_of_pid(pgrp));

         found = 1;
         break;
      }
   }
   rcu_read_unlock();

   if(!found) {
      printk("Cannot find the process. Giving up\n");
      return -1;
   }
#endif

   for(nr_samples_generated = 0; nr_samples_generated < nr_samples_to_generate; nr_samples_generated ++) {
      struct ibs_op_sample ibs_op;
      unsigned int cpu = 0;
      unsigned char which_node;
      unsigned long min_phys_address;
      unsigned long max_phys_address;

      unsigned long rand_lin_interval = max_lin_address - min_lin_address;

      get_random_bytes(&ibs_op.phys_addr, sizeof (ibs_op.phys_addr));
      get_random_bytes(&ibs_op.lin_addr, sizeof (ibs_op.lin_addr));
      get_random_bytes(&which_node, sizeof (which_node));

#if ! TEST_REPLICATION
      get_random_bytes(&cpu, sizeof (cpu));
      cpu = cpu % num_online_cpus();
#endif



      ibs_op.lin_addr = min_lin_address + (ibs_op.lin_addr % rand_lin_interval);

      which_node = which_node % ndies;
      max_phys_address = node2physend(which_node);
      if(which_node == 0) {
         min_phys_address = 0;
      }
      else {
         min_phys_address = node2physend(which_node-1) + 1;
      }

      /*printk("rand_lin_interval = %lu, rand_phy_interval = %lu, ndies = %d, pid stuff = %lu, npids = %lu\n", 
               rand_lin_interval, (max_phys_address - min_phys_address), ndies, (PID_MAX_DEFAULT - RESERVED_PIDS), npids); */

      ibs_op.phys_addr = min_phys_address + (ibs_op.phys_addr % (max_phys_address - min_phys_address));


#ifndef APP_TO_CONSIDER
      memset(comm, 0, TASK_COMM_LEN);
      rcu_read_lock();
      do {
         get_random_bytes(&pid, sizeof (pid));
         pid = RESERVED_PIDS + (pid % (PID_MAX_DEFAULT - RESERVED_PIDS));

         task = find_task_by_vpid_hook(pid);
      } while(task == NULL);

      tgid = task_tgid_nr_ns(task, ns_of_pid(find_get_pid(pid)));
      get_task_comm(comm, task);
      rcu_read_unlock();
#else
      get_random_bytes(&pid, sizeof (pid));
      pid = pid % npids;
      pid = pids[pid];
#endif
     
      /** These values might be used **/
      ibs_op.data3.IbsDcMissLat = 1000;
      ibs_op.data3.IbsStOp = 0;
      ibs_op.data3.IbsLdOp = 1;

      //printk("Generating a new sample (cpu = %d, phys = 0x%lx, lin = 0x%lx, pid = %u, tgid = %u, name = %s)\n", cpu, (unsigned long) ibs_op.phys_addr, (unsigned long) ibs_op.lin_addr, pid, tgid, comm);
#if TEST_REPLICATION
      for_each_online_cpu(cpu) {
         rbtree_add_sample(0, &ibs_op, cpu, pid, tgid);
      }
#else
      rbtree_add_sample(0, &ibs_op, cpu, pid, tgid);
#endif
   }

   return 1; // report success
}

int ibs_init(void) {
   printk("(WARNING) Fake IBS selected\n");
   return 0;
}

void ibs_exit(void) {
   printk("(WARNING) Fake IBS selected -- BYE\n");
}

void ibs_start() {
   work_thread = kthread_run(handle_ibs_fake, NULL, "fakeibsd");
   if(IS_ERR(work_thread)) {
      printk(KERN_ALERT "Fake ibs thread creation failed\n");
   }
}

int ibs_stop() {
   // TODO: join thread
   printk("Total (fake) Samples %lu\n", nr_samples_generated);
   //rbtree_print();
   return nr_samples_generated;
}
#endif

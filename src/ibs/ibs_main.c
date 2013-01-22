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
#include "ibs_defs.h"
#include "ibs_init.h"
#include "nmi_int.h"

#if ! FAKE_IBS

//unsigned sampling_rate = 0x1FFF0;
//unsigned dispatched_ops = 0; //Sample cycles (0) or operations (1)

//unsigned sampling_rate = 0x1FFF;
//unsigned dispatched_ops = 1; //Sample cycles (0) or operations (1)

//unsigned sampling_rate = 0x1FFF0;
//unsigned dispatched_ops = 0; //Sample cycles (0) or operations (1)

//unsigned sampling_rate = 0x7FFC;
//unsigned sampling_rate = 0xFFF8;

#if ADAPTIVE_SAMPLING
//unsigned sampling_rate_accurate = 0x3FFE;
//unsigned sampling_rate_accurate = 0x7FFC;
unsigned sampling_rate_accurate  = 0xFFF8;
//unsigned sampling_rate_accurate = 0x1FFF0;
unsigned sampling_rate_cheap    = 0x3FFE0;
unsigned sampling_rate;
#else
unsigned sampling_rate           = 0x1FFF0;
#endif

int consider_L1L2 = 0;
static int dispatched_ops = 0; //Sample cycles (0) or operations (1)

unsigned long min_lin_address = 0;
unsigned long max_lin_address = (unsigned long) (-1);

struct ibs_stats {
   uint64_t total_interrupts;
   uint64_t total_samples;
   uint64_t total_samples_L3DRAM;
   uint64_t total_sample_overflow;
#if DUMP_OVERHEAD
   uint64_t time_spent_in_NMI;
#endif
};
static DEFINE_PER_CPU(struct ibs_stats, stats);

// This function creates a struct ibs_op_sample struct that contains all IBS info
// This structure is passed to rbtree_add_sample which stores pages info in a rbreee.
static int handle_ibs_nmi(struct pt_regs * const regs) {
   unsigned int low, high;
   struct ibs_op_sample ibs_op_stack;
   struct ibs_op_sample *ibs_op = &ibs_op_stack;
   int cpu = smp_processor_id();
#if DUMP_OVERHEAD
   uint64_t time_start,time_stop;
   rdtscll(time_start);
#endif

   per_cpu(stats, cpu).total_interrupts++;

   rdmsr(MSR_AMD64_IBSOPCTL, low, high);
   if (low & IBS_OP_LOW_VALID_BIT) {
      rdmsr(MSR_AMD64_IBSOPDATA2, low, high);
      ibs_op->ibs_op_data2_low = low;
      ibs_op->ibs_op_data2_high = high;

      // If the sample does not touch DRAM, stop.
      /*if((ibs_op->ibs_op_data2_low & 7) != 3) {
	      goto end;
      }*/
      if((!consider_L1L2) && ((ibs_op->ibs_op_data2_low & 7) == 0)) {
	      goto end;
      }
      if((ibs_op->ibs_op_data2_low & 7) == 3) 
	      per_cpu(stats, cpu).total_samples_L3DRAM++;

      rdmsr(MSR_AMD64_IBSOPRIP, low, high);
      ibs_op->ibs_op_rip_low = low;
      ibs_op->ibs_op_rip_high = high;
      rdmsr(MSR_AMD64_IBSOPDATA, low, high);
      ibs_op->ibs_op_data1_low = low;
      ibs_op->ibs_op_data1_high = high;
      rdmsr(MSR_AMD64_IBSOPDATA3, low, high);
      ibs_op->ibs_op_data3_low = low;
      ibs_op->ibs_op_data3_high = high;
      rdmsr(MSR_AMD64_IBSDCLINAD, low, high);
      ibs_op->ibs_dc_linear_low = low;
      ibs_op->ibs_dc_linear_high = high;
      rdmsr(MSR_AMD64_IBSDCPHYSAD, low, high);
      ibs_op->ibs_dc_phys_low = low;
      ibs_op->ibs_dc_phys_high = high;
      
      if(ibs_op->phys_addr == 0)
	      goto end;

      if(ibs_op->lin_addr < min_lin_address || ibs_op->lin_addr > max_lin_address) {
         goto end;
      }

      per_cpu(stats, cpu).total_samples++;

      rbtree_add_sample(!user_mode(regs), ibs_op, smp_processor_id(), current->pid, current->tgid);

end: __attribute__((unused));
      rdmsr(MSR_AMD64_IBSOPCTL, low, high);
      high = 0;
      low &= ~IBS_OP_LOW_VALID_BIT;
      low |= IBS_OP_LOW_ENABLE;
#if DUMP_OVERHEAD
      rdtscll(time_stop);
      per_cpu(stats, cpu).time_spent_in_NMI += time_stop - time_start;
#endif
      wrmsr(MSR_AMD64_IBSOPCTL, low, high);
   }


exit: __attribute__((unused));
   return 1; // report success
}

/**
 * The next 4 functions are passed to nmi_int.c to start/stop IBS (low level)
 * start and stop are called on each CPU
 */
static void __ibs_start(void) {
   set_ibs_rate(sampling_rate, dispatched_ops);
}

static void __ibs_stop(void) {
   unsigned int low, high;
   low = 0;		// clear max count and enable
   high = 0;
   wrmsr(MSR_AMD64_IBSOPCTL, low, high);
}

static void __ibs_shutdown(void) {
   on_each_cpu(apic_clear_ibs_nmi_per_cpu, NULL, 1);
}

static void __ibs_setup(void) {
}

static struct ibs_model model = {
   .setup = __ibs_setup,
   .shutdown = __ibs_shutdown,
   .start = __ibs_start,
   .stop = __ibs_stop,
   .check_ctrs = handle_ibs_nmi,
};


/**
 * Init and exit
 */
int ibs_init(void) {
   int err;

   if (!boot_cpu_has(X86_FEATURE_IBS)) {
      printk(KERN_ERR "ibs: AMD IBS not present in hardware\n");
      return -ENODEV;
   }

#if ADAPTIVE_SAMPLING
   sampling_rate = sampling_rate_cheap;
#endif

   printk(KERN_INFO "ibs: AMD IBS detected\n");

   err = ibs_nmi_init(&model);
   if (err)
      return err;

   err = ibs_nmi_setup();
   if(err) {
      printk("ibs_nmi_setup() error %d\n", err);
      return err;
   }
   pfm_amd64_setup_eilvt();

   return 0;
}

void ibs_exit(void) {
   ibs_nmi_shutdown();
   ibs_nmi_exit();
}

void ibs_start() {
   int cpu;
   for_each_possible_cpu(cpu) {
      memset(&per_cpu(stats, cpu), 0, sizeof(per_cpu(stats, cpu)));
   }

   ibs_nmi_start();
}

#if DUMP_OVERHEAD
uint64_t time_spent_in_NMI = 0;
#endif
int ibs_stop() {
   int cpu, total_interrupts = 0, total_samples = 0, total_samples_L3DRAM = 0;
#if DUMP_OVERHEAD
   time_spent_in_NMI = 0;
#endif

   ibs_nmi_stop();

   for_each_possible_cpu(cpu) {
	   total_interrupts += per_cpu(stats, cpu).total_interrupts;
	   total_samples += per_cpu(stats, cpu).total_samples;
	   total_samples_L3DRAM += per_cpu(stats, cpu).total_samples_L3DRAM;
#if DUMP_OVERHEAD
	   time_spent_in_NMI += per_cpu(stats, cpu).time_spent_in_NMI;
#endif
   }
   printk("Sampling: %x op=%d considering L1&L2=%d\n", sampling_rate, dispatched_ops, consider_L1L2);
   printk("Total interrupts %d Total Samples %d\n", total_interrupts, total_samples);

   return total_samples_L3DRAM;
}

#endif

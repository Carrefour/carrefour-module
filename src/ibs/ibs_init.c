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
#include "nmi_int.h"

#if ! FAKE_IBS

/**
 * Various functions that were not provided by AMD.
 * Set the IBS rate and do what is necessarily in the APIC to init IBS
 */
void set_ibs_rate(int cnt, int ops) {
   unsigned int low, high;
   uint32_t rand = 0;
   low = (((cnt + rand) >> 4) & 0xFFFF)
      + ((ops & 0x1) << 19) // bit 19
      + IBS_OP_LOW_ENABLE;
   high = 0;
   wrmsr(MSR_AMD64_IBSOPCTL, low, high);
}


static u8 ibs_eilvt_off;
static void my_setup_APIC_eilvt(u8 lvt_off, u8 vector, u8 msg_type, u8 mask)
{
#  define APIC_EILVT0     0x500
   unsigned long reg = (lvt_off << 4) + APIC_EILVT0;
   unsigned int  v   = (mask << 16) | (msg_type << 8) | vector;

   apic_write(reg, v);
}
u8 setup_APIC_eilvt_ibs(u8 vector, u8 msg_type, u8 mask)
{
#  define APIC_EILVT_LVTOFF_IBS 1
   my_setup_APIC_eilvt(APIC_EILVT_LVTOFF_IBS, vector, msg_type, mask);
   return APIC_EILVT_LVTOFF_IBS;
}
static inline void apic_init_ibs_nmi_per_cpu(void *arg)
{
   ibs_eilvt_off = setup_APIC_eilvt_ibs(0, APIC_EILVT_MSG_NMI, 0);
}

void apic_clear_ibs_nmi_per_cpu(void *arg)
{
   setup_APIC_eilvt_ibs(0, APIC_EILVT_MSG_FIX, 1);
}

int pfm_amd64_setup_eilvt(void)
{
#  define IBSCTL_LVTOFFSETVAL		(1 << 8)
#  define IBSCTL				0x1cc
   struct pci_dev *cpu_cfg;
   int nodes;
   u32 value = 0;

   /* per CPU setup */
   on_each_cpu(apic_init_ibs_nmi_per_cpu, NULL, 1);

   nodes = 0;
   cpu_cfg = NULL;
   do {
      cpu_cfg = pci_get_device(PCI_VENDOR_ID_AMD,
            PCI_DEVICE_ID_AMD_10H_NB_MISC,
            cpu_cfg);
      if (!cpu_cfg)
         break;
      ++nodes;
      pci_write_config_dword(cpu_cfg, IBSCTL, ibs_eilvt_off
            | IBSCTL_LVTOFFSETVAL);
      pci_read_config_dword(cpu_cfg, IBSCTL, &value);
      if (value != (ibs_eilvt_off | IBSCTL_LVTOFFSETVAL)) {
         printk(KERN_DEBUG "Failed to setup IBS LVT offset, "
               "IBSCTL = 0x%08x\n", value);
         return 1;
      }
   } while (1);

   if (!nodes) {
      printk(KERN_DEBUG "No CPU node configured for IBS\n");
      return 1;
   }

#ifdef CONFIG_NUMA
   /* Sanity check */
   /* Works only for 64bit with proper numa implementation. */
   if (nodes != num_possible_nodes()) {
      printk(KERN_DEBUG "Failed to setup CPU node(s) for IBS, "
            "found: %d, expected %d\n",
            nodes, num_possible_nodes());
      return 1;
   }
#endif
   return 0;
}

#endif

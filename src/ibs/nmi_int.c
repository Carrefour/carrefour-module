/**
 * @file nmi_int.c
 *
 * @remark Copyright 2002-2008 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon <levon@movementarian.org>
 * @author Robert Richter <robert.richter@amd.com>
 */

#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/smp.h>
#include <linux/oprofile.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include <linux/kdebug.h>
#include <linux/cpu.h>
#ifdef CONFIG_PERFMON
#include <linux/perfmon_kern.h>
#endif
#include <asm/nmi.h>
#include <asm/msr.h>
#include <asm/apic.h>

#include "carrefour_main.h"
#include "nmi_int.h"

#if ! FAKE_IBS
static DEFINE_PER_CPU(unsigned long, saved_lvtpc);
static struct ibs_model *model;

/* 0 == registered but off, 1 == registered and on */
static int nmi_enabled = 0;

/*static int profile_exceptions_notify(struct notifier_block *self,
				     unsigned long val, void *data)
{
	struct die_args *args = (struct die_args *)data;
	int ret = NOTIFY_DONE;
   printk("profile_exceptions_notify\n");

	switch (val) {
	case DIE_NMI:
		if (model->check_ctrs(args->regs))
			ret = NOTIFY_STOP;
		break;
	default:
		break;
	}
	return ret;
}*/

static void nmi_cpu_setup(void *dummy)
{
	int cpu = smp_processor_id();
	model->setup();
	per_cpu(saved_lvtpc, cpu) = apic_read(APIC_LVTPC);
	apic_write(APIC_LVTPC, APIC_DM_NMI);
}

/*static struct notifier_block profile_exceptions_nb = {
	.notifier_call = profile_exceptions_notify,
	.next = NULL,
	.priority = 0
};*/

#ifndef NMI_HANDLED
#define NMI_DONE        0
#define NMI_HANDLED     1
#define NMI_LOCAL 0
#define register_nmi_handler(...) printk("Update your kernel!\n");
#define unregister_nmi_handler(...)
#endif
static int __attribute__((unused))
ibs_event_nmi_handler(unsigned int cmd, struct pt_regs *regs)
{
	if (nmi_enabled && model->check_ctrs(regs))
      return NMI_HANDLED;
   return NMI_DONE;
}

int ibs_nmi_setup(void)
{
	int err = 0;

#ifdef CONFIG_PERFMON
	if (pfm_session_allcpus_acquire())
		return -EBUSY;
#endif

   printk("ibs_nmi_setup\n");
	/*err = register_die_notifier(&profile_exceptions_nb);
	if (err) {
#ifdef CONFIG_PERFMON
		pfm_session_allcpus_release();
#endif
		return err;
	}*/

   err = register_nmi_handler(NMI_LOCAL, ibs_event_nmi_handler, 0, "carrouf");

	/* We need to serialize save and setup for HT because the subset
	 * of msrs are distinct for save and setup operations
	 */
	on_each_cpu(nmi_cpu_setup, NULL, 1);
	nmi_enabled = 1;
	return 0;
}

static void nmi_cpu_shutdown(void *dummy)
{
	unsigned int v;
	int cpu = smp_processor_id();

	/* restoring APIC_LVTPC can trigger an apic error because the delivery
	 * mode and vector nr combination can be illegal. That's by design: on
	 * power on apic lvt contain a zero vector nr which are legal only for
	 * NMI delivery mode. So inhibit apic err before restoring lvtpc
	 */
	v = apic_read(APIC_LVTERR);
	apic_write(APIC_LVTERR, v | APIC_LVT_MASKED);
	apic_write(APIC_LVTPC, per_cpu(saved_lvtpc, cpu));
	apic_write(APIC_LVTERR, v);
}

void ibs_nmi_shutdown(void)
{
	nmi_enabled = 0;
	on_each_cpu(nmi_cpu_shutdown, NULL, 1);
	//unregister_die_notifier(&profile_exceptions_nb);
   unregister_nmi_handler(NMI_LOCAL, "carrouf");
	model->shutdown();
#ifdef CONFIG_PERFMON
	pfm_session_allcpus_release();
#endif
}

static void nmi_cpu_start(void *dummy)
{
	model->start();
}

int ibs_nmi_start(void)
{
	on_each_cpu(nmi_cpu_start, NULL, 1);
	return 0;
}

static void nmi_cpu_stop(void *dummy)
{
	model->stop();
}

void ibs_nmi_stop(void)
{
	on_each_cpu(nmi_cpu_stop, NULL, 1);
}

#ifdef CONFIG_SMP
static int ibs_cpu_notifier(struct notifier_block *b, unsigned long action,
				 void *data)
{
	int cpu = (unsigned long)data;
   printk("ibs_cpu_notifier\n");
	switch (action) {
	case CPU_DOWN_FAILED:
	case CPU_ONLINE:
		smp_call_function_single(cpu, nmi_cpu_start, NULL, 0);
		break;
	case CPU_DOWN_PREPARE:
		smp_call_function_single(cpu, nmi_cpu_stop, NULL, 1);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block ibs_cpu_nb = {
	.notifier_call = ibs_cpu_notifier
};
#endif

int ibs_nmi_init(struct ibs_model *m)
{
   int err = 0;
	if (!cpu_has_apic)
		return -ENODEV;

	model = m;

#ifdef CONFIG_SMP
	err = register_cpu_notifier(&ibs_cpu_nb);
   if(err) {
      printk("Cannot register cpu_notifier\n");
      return -EBUSY;
   }
#endif

	printk(KERN_INFO "sdp: using NMI interrupt.\n");
	return 0;
}

void ibs_nmi_exit(void)
{
#ifdef CONFIG_SMP
	unregister_cpu_notifier(&ibs_cpu_nb);
#endif
}

#endif

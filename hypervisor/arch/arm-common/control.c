/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) ARM Limited, 2014
 * Copyright (c) Siemens AG, 2016
 *
 * Authors:
 *  Jean-Philippe Brucker <jean-philippe.brucker@arm.com>
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/control.h>
#include <jailhouse/printk.h>
#include <asm/control.h>
#include <asm/psci.h>

static void enter_cpu_off(struct per_cpu *cpu_data)
{
	cpu_data->park = false;
	cpu_data->wait_for_poweron = true;
}

void arm_cpu_park(void)
{
	struct per_cpu *cpu_data = this_cpu_data();

	spin_lock(&cpu_data->control_lock);
	enter_cpu_off(cpu_data);
	spin_unlock(&cpu_data->control_lock);

	arm_cpu_reset(0);
	arm_paging_vcpu_init(&parking_mm);
}

void arm_cpu_kick(unsigned int cpu_id)
{
	struct sgi sgi = {};

	sgi.targets = 1 << cpu_id;
	sgi.id = SGI_EVENT;
	irqchip_send_sgi(&sgi);
}

void arch_suspend_cpu(unsigned int cpu_id)
{
	struct per_cpu *target_data = per_cpu(cpu_id);
	bool target_suspended;

	spin_lock(&target_data->control_lock);

	target_data->suspend_cpu = true;
	target_suspended = target_data->cpu_suspended;

	spin_unlock(&target_data->control_lock);

	if (!target_suspended) {
		arm_cpu_kick(cpu_id);

		while (!target_data->cpu_suspended)
			cpu_relax();
	}
}

void arch_resume_cpu(unsigned int cpu_id)
{
	struct per_cpu *target_data = per_cpu(cpu_id);

	/* take lock to avoid theoretical race with a pending suspension */
	spin_lock(&target_data->control_lock);

	target_data->suspend_cpu = false;

	spin_unlock(&target_data->control_lock);
}

void arch_reset_cpu(unsigned int cpu_id)
{
	per_cpu(cpu_id)->reset = true;

	arch_resume_cpu(cpu_id);
}

void arch_park_cpu(unsigned int cpu_id)
{
	per_cpu(cpu_id)->park = true;

	arch_resume_cpu(cpu_id);
}

static void check_events(struct per_cpu *cpu_data)
{
	bool reset = false;

	spin_lock(&cpu_data->control_lock);

	do {
		if (cpu_data->suspend_cpu)
			cpu_data->cpu_suspended = true;

		spin_unlock(&cpu_data->control_lock);

		while (cpu_data->suspend_cpu)
			cpu_relax();

		spin_lock(&cpu_data->control_lock);

		if (!cpu_data->suspend_cpu) {
			cpu_data->cpu_suspended = false;

			if (cpu_data->park) {
				enter_cpu_off(cpu_data);
				break;
			}

			if (cpu_data->reset) {
				cpu_data->reset = false;
				if (cpu_data->cpu_on_entry !=
				    PSCI_INVALID_ADDRESS) {
					cpu_data->wait_for_poweron = false;
					reset = true;
				} else {
					enter_cpu_off(cpu_data);
				}
				break;
			}
		}
	} while (cpu_data->suspend_cpu);

	if (cpu_data->flush_vcpu_caches) {
		cpu_data->flush_vcpu_caches = false;
		arm_paging_vcpu_flush_tlbs();
	}

	spin_unlock(&cpu_data->control_lock);

	/*
	 * wait_for_poweron is only modified on this CPU, so checking outside of
	 * control_lock is fine.
	 */
	if (cpu_data->wait_for_poweron)
		arm_cpu_park();
	else if (reset)
		arm_cpu_reset(cpu_data->cpu_on_entry);
}

void arch_handle_sgi(struct per_cpu *cpu_data, u32 irqn,
		     unsigned int count_event)
{
	switch (irqn) {
	case SGI_INJECT:
		cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_VSGI] += count_event;
		irqchip_inject_pending(cpu_data);
		break;
	case SGI_EVENT:
		cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_MANAGEMENT] +=
			count_event;
		check_events(cpu_data);
		break;
	default:
		printk("WARN: unknown SGI received %d\n", irqn);
	}
}

/*
 * Handle the maintenance interrupt, the rest is injected into the cell.
 * Return true when the IRQ has been handled by the hyp.
 */
bool arch_handle_phys_irq(struct per_cpu *cpu_data, u32 irqn,
			  unsigned int count_event)
{
	if (irqn == system_config->platform_info.arm.maintenance_irq) {
		cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_MAINTENANCE] +=
			count_event;
		irqchip_inject_pending(cpu_data);

		return true;
	}

	cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_VIRQ] += count_event;
	irqchip_set_pending(cpu_data, irqn);

	return false;
}

void arch_cell_reset(struct cell *cell)
{
	arm_cell_dcaches_flush(cell, DCACHE_INVALIDATE);
}

/* Note: only supports synchronous flushing as triggered by config_commit! */
void arch_flush_cell_vcpu_caches(struct cell *cell)
{
	unsigned int cpu;

	for_each_cpu(cpu, cell->cpu_set)
		if (cpu == this_cpu_id())
			arm_paging_vcpu_flush_tlbs();
		else
			per_cpu(cpu)->flush_vcpu_caches = true;
}

void arch_config_commit(struct cell *cell_added_removed)
{
	irqchip_config_commit(cell_added_removed);
}

void __attribute__((noreturn)) arch_panic_stop(void)
{
	asm volatile ("1: wfi; b 1b");
	__builtin_unreachable();
}

void arch_panic_park(void) __attribute__((alias("arm_cpu_park")));

void arch_shutdown(void)
{
}

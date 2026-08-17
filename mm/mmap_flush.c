/*
 * mm/mmap_flush.c - remote TLB/icache shootdown
 */

#include <nuvix/mm.h>
#include <nuvix/cpu.h>
#include <nuvix/ipi.h>
#include <nuvix/sched.h>

void mm_flush_remote(struct mm_struct *mm, bool flush_icache)
{
	uint32_t self_id = current_cpu()->id;
	uint64_t schedulable = cpu_schedulable_mask();
	int reasons = IPI_SHOOTDOWN;

	if (flush_icache)
		reasons |= IPI_FENCE_I;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (id == self_id || !(schedulable & (1ULL << id)))
			continue;
		if (sched_cpu_active_mm(id) == mm)
			ipi_send_shootdown(id, reasons);
	}
}

void mm_flush_kernel_all(void)
{
	uint32_t self_id = current_cpu()->id;
	uint64_t schedulable = cpu_schedulable_mask();
	uint64_t targets;

	/* Exclude self from targets. */
	targets = schedulable & ~(1ULL << self_id);

	/* If no other CPUs are schedulable, skip shootdown: either we're in
	 * early boot (before SMP gate) or all secondaries are idle. Also skip
	 * if we're not schedulable ourselves (e.g., secondary CPU during boot
	 * before the gate). */
	if (targets == 0 || !(schedulable & (1ULL << self_id)))
		return;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (targets & (1ULL << id))
			ipi_send_shootdown(id, IPI_SHOOTDOWN);
	}
}

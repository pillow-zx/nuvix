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

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (id == self_id || !(schedulable & (1ULL << id)))
			continue;
		ipi_send_shootdown(id, IPI_SHOOTDOWN);
	}
}

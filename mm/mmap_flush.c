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
	int reasons = IPI_SHOOTDOWN;

	if (flush_icache)
		reasons |= IPI_FENCE_I;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (id == self_id || !cpu_is_online(id))
			continue;
		if (sched_cpu_active_mm(id) == mm)
			ipi_send_shootdown(id, reasons);
	}
}

void mm_flush_kernel_all(void)
{
	uint32_t self_id = current_cpu()->id;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (id == self_id || !cpu_is_online(id))
			continue;
		ipi_send_shootdown(id, IPI_SHOOTDOWN);
	}
}

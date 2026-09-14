/*
 * arch/riscv/mm/shootdown.c - remote TLB/icache shootdown
 */

#include <nuvix/mm.h>
#include <nuvix/cpu.h>
#include <nuvix/ipi.h>


void mm_flush_remote(struct mm_struct *mm, bool flush_icache)
{
	uint32_t self_id = current_cpu()->id;
	uint64_t online = cpu_online_mask();
	int reasons = IPI_SHOOTDOWN;

	/* No ASIDs: every entry into a user root flushes locally. Broadcast
	 * covers CPUs already running it, including concurrent handoffs,
	 * without borrowing a scheduler snapshot as translation ownership. */
	(void)mm;
	if (flush_icache)
		reasons |= IPI_FENCE_I;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (id == self_id || !(online & (1ULL << id)))
			continue;
		ipi_send_sync(id, reasons);
	}
}

void mm_flush_kernel_all(void)
{
	uint32_t self_id = current_cpu()->id;
	uint64_t online = cpu_online_mask();
	uint64_t targets;

	/* Exclude self from targets. */
	targets = online & ~(1ULL << self_id);

	if (targets == 0)
		return;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (targets & (1ULL << id))
			ipi_send_sync(id, IPI_SHOOTDOWN);
	}
}

/*
 * arch/riscv/mm/shootdown.c - remote TLB/icache shootdown
 */

#include <nuvix/mm.h>
#include <nuvix/cpu.h>
#include <nuvix/printk.h>
#include <arch/sbi.h>

static uint64_t online_hart_mask(void)
{
	uint64_t online = cpu_online_mask();
	uint64_t harts = 0;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		uint32_t hartid;

		if (!(online & BIT_U64(id)))
			continue;
		hartid = cpu_table[id].hartid;
		BUG_ON(hartid >= sizeof(harts) * 8);
		harts |= BIT_U64(hartid);
	}
	return harts;
}

void mm_flush_remote(struct mm_struct *mm, bool flush_icache)
{
	struct sbi_ret ret;
	uint64_t harts;

	/* No ASIDs: every entry into a user root flushes locally. Broadcast
	 * covers every online hart, including the caller, without borrowing a
	 * scheduler snapshot as translation ownership. Including the caller keeps
	 * the operation correct if this task migrates around the local flush. */
	(void)mm;
	harts = online_hart_mask();
	if (!harts)
		return;

	ret = sbi_remote_sfence_vma(harts, 0, 0, 0);
	if (ret.error != 0)
		panic("sbi: remote SFENCE.VMA failed (harts=0x%lx error=%ld)",
		      harts, ret.error);
	if (!flush_icache)
		return;
	ret = sbi_remote_fence_i(harts, 0);
	if (ret.error != 0)
		panic("sbi: remote FENCE.I failed (harts=0x%lx error=%ld)",
		      harts, ret.error);
}

void mm_flush_all(void)
{
	struct sbi_ret ret;
	uint64_t harts = online_hart_mask();

	if (!harts)
		return;
	ret = sbi_remote_sfence_vma(harts, 0, 0, 0);
	if (ret.error != 0)
		panic("sbi: remote SFENCE.VMA failed (harts=0x%lx error=%ld)",
		      harts, ret.error);
}

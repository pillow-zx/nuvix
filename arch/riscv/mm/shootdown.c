/*
 * arch/riscv/mm/shootdown.c - remote TLB/icache shootdown
 */

#include <nuvix/mm.h>
#include <nuvix/cpu.h>
#include <nuvix/printk.h>
#include <asm/sbi.h>

static void flush_online(bool flush_icache)
{
	uint64_t pending = cpu_online_mask();
	while (pending) {
		unsigned first = ctzll(pending);
		uint64_t base = (uint64_t)cpu_table[first].hartid & ~63ULL;
		uint64_t harts = 0;
		for (uint32_t id = 0; id < nr_cpu_ids; id++) {
			uint64_t hart = cpu_table[id].hartid;
			if (!(pending & BIT_U64(id)) || (hart & ~63ULL) != base)
				continue;
			harts |= BIT_U64(hart - base);
			pending &= ~BIT_U64(id);
		}
		struct sbi_ret ret = sbi_remote_sfence_vma(harts, base, 0, 0);
		if (ret.error)
			panic("sbi: remote SFENCE.VMA failed (base=%lu mask=%lx error=%ld)",
			      base, harts, ret.error);
		if (flush_icache) {
			ret = sbi_remote_fence_i(harts, base);
			if (ret.error)
				panic("sbi: remote FENCE.I failed (base=%lu mask=%lx error=%ld)",
				      base, harts, ret.error);
		}
	}
}

void mm_flush_remote(struct mm_struct *mm, bool flush_icache)
{
	/* No ASIDs: every entry into a user root flushes locally. Broadcast
	 * covers every online hart, including the caller, without borrowing a
	 * scheduler snapshot as translation ownership. Including the caller keeps
	 * the operation correct if this task migrates around the local flush. */
	(void)mm;
	flush_online(flush_icache);
}

void mm_flush_all(void)
{
	flush_online(false);
}

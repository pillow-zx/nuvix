/*
 * arch/riscv/mm/tlb.c - TLB 刷新 (sfence.vma)
 */

#include <nuvix/types.h>
#include <asm/csr.h>

__always_inline
static inline void local_sfence_vma(uintptr_t addr, uintptr_t asid)
{
	asm volatile("sfence.vma %0, %1" : : "r"(addr), "r"(asid) : "memory");
}

void tlb_flush_all(void)
{

	local_sfence_vma(0, 0);
}

void tlb_flush_page(uintptr_t va)
{
	local_sfence_vma(va, 0);
}

void icache_flush(void)
{
	asm volatile("fence.i" : : : "memory");
}

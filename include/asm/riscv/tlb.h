#ifndef _ASM_RISCV_TLB_H
#define _ASM_RISCV_TLB_H

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>

static __always_inline void arch_local_flush_tlb_all(void)
{
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

/* For leaf updates; non-leaf updates require a full flush. */
static __always_inline void arch_local_flush_tlb_page(vaddr_t addr)
{
    __asm__ volatile("sfence.vma %0, zero" : : "r"(addr) : "memory");
}

static __always_inline void arch_local_flush_icache_all(void)
{
    __asm__ volatile("fence.i" ::: "memory");
}

#ifdef CONFIG_SMP
void arch_remote_flush_tlb_all(void);
void arch_remote_flush_tlb_page(vaddr_t addr);
void arch_remote_flush_icache_all(void);

#endif /* CONFIG_SMP */

#endif /* _ASM_RISCV_TLB_H */

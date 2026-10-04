#ifndef _NUVIX_RISCV_TLB_H
#define _NUVIX_RISCV_TLB_H

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>

static __always_inline inline void arch_local_flush_tlb_all(void)
{
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

/* For leaf updates; non-leaf updates require a full flush. */
static __always_inline inline void arch_local_flush_tlb_page(vaddr_t addr)
{
    __asm__ volatile("sfence.vma %0, zero" : : "r"(addr) : "memory");
}

#endif /* _NUVIX_RISCV_TLB_H */

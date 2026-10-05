#ifndef _NUVIX_RISCV_TLB_H
#define _NUVIX_RISCV_TLB_H

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>
#include <nuvix/cpu.h>
#include <asm/sbi.h>

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

/* Publish page-table or code writes before requesting a remote fence. */
static __always_inline long arch_remote_flush_tlb_all(unsigned int cpu_id)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
    return sbi_remote_sfence_vma(1UL, cpus[cpu_id].hart_id, 0, 0).error;
}

static __always_inline long arch_remote_flush_icache_all(unsigned int cpu_id)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
    return sbi_remote_fence_i(1UL, cpus[cpu_id].hart_id).error;
}

#endif /* _NUVIX_RISCV_TLB_H */

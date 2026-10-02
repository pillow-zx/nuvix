#ifndef _NUVIX_RISCV_IRQ_H
#define _NUVIX_RISCV_IRQ_H

#define SSTATUS_SIE (1 << 1)

#ifndef __ASSEMBLER__

#include <nuvix/compiler.h>

static __always_inline inline unsigned long arch_local_irq_save(void)
{
    unsigned long flags;

    __asm__ volatile("csrrc %0, sstatus, %1"
                     : "=r"(flags)
                     : "rK"(SSTATUS_SIE)
                     : "memory");
    return flags & SSTATUS_SIE;
}

static __always_inline inline void arch_local_irq_restore(unsigned long flags)
{
    /* Restore only SIE; other sstatus fields may have changed meanwhile. */
    if (flags & SSTATUS_SIE)
        __asm__ volatile("csrs sstatus, %0" : : "rK"(SSTATUS_SIE) : "memory");
    else
        __asm__ volatile("csrc sstatus, %0" : : "rK"(SSTATUS_SIE) : "memory");
}

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_IRQ_H */

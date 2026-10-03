#ifndef _NUVIX_RISCV_IRQ_H
#define _NUVIX_RISCV_IRQ_H

#include <nuvix/arch/riscv/csr.h>

#ifndef __ASSEMBLER__

#include <nuvix/compiler.h>

static __always_inline inline unsigned long arch_local_irq_save(void)
{
    unsigned long flags = csr_read_clear(sstatus, SSTATUS_SIE);
    return flags & SSTATUS_SIE;
}

static __always_inline inline void arch_local_irq_restore(unsigned long flags)
{
    /* Restore only SIE; other sstatus fields may have changed meanwhile. */
    if (flags & SSTATUS_SIE)
        csr_set(sstatus, SSTATUS_SIE);
    else
        csr_clear(sstatus, SSTATUS_SIE);
}

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_IRQ_H */

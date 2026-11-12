#ifndef _NUVIX_RISCV_IRQ_H
#define _NUVIX_RISCV_IRQ_H

#include <asm/csr.h>

#ifndef __ASSEMBLER__

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>

/* Boot-only; leaves sources disabled and returns the highest valid IRQ ID. */
int arch_irq_init(unsigned int *nr_irqs);
int arch_irq_parse(int node, unsigned int *irq);
int arch_irq_enable(unsigned int irq);
int arch_irq_disable(unsigned int irq);

/* CPU0, hard IRQ context with local IRQs off. */
void arch_handle_external_irq(void);

static __always_inline bool arch_local_irqs_disabled(void)
{
    return !(csr_read(sstatus) & SSTATUS_SIE);
}

static __always_inline void arch_local_irq_disable(void)
{
    csr_clear(sstatus, SSTATUS_SIE);
}

static __always_inline void arch_local_irq_enable(void)
{
    csr_set(sstatus, SSTATUS_SIE);
}

static __always_inline unsigned long arch_local_irq_save(void)
{
    unsigned long flags = csr_read_clear(sstatus, SSTATUS_SIE);
    return flags & SSTATUS_SIE;
}

static __always_inline void arch_local_irq_restore(unsigned long flags)
{
    /* Restore only SIE; other sstatus fields may have changed meanwhile. */
    if (flags & SSTATUS_SIE)
        arch_local_irq_enable();
    else
        arch_local_irq_disable();
}

/* Boot-only after buddy_init(), before SMP; leaves all sources disabled. */
int plic_init(void);

/* Boot-only after init; supports one source from this PLIC per device node. */
int plic_irq_parse(int node, unsigned int *irq);

/* After init; highest valid source ID, with sources numbered from one. */
unsigned int plic_nr_irqs(void);

/* After init; routes only to CPU0. Returns -EINVAL for an invalid source. */
int plic_enable(unsigned int irq);
/* Masks delivery; does not drain handlers or stop device activity. */
int plic_disable(unsigned int irq);

/* CPU0, hard IRQ context with local IRQs off. Zero means no pending source. */
unsigned int plic_claim(void);
/* Complete a nonzero source returned by claim, after handling device state. */
void plic_complete(unsigned int irq);

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_IRQ_H */

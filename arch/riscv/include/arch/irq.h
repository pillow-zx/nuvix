#ifndef _NUVIX_ARCH_RISCV_IRQ_H
#define _NUVIX_ARCH_RISCV_IRQ_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <asm/csr.h>

typedef unsigned long irq_flags_t;

__always_inline __must_check
static inline irq_flags_t local_irq_save(void)
{
	irq_flags_t flags = csr_read(sstatus);

	csr_clear(sstatus, SSTATUS_SIE);
	return flags;
}

__always_inline
static inline void local_irq_restore(irq_flags_t flags)
{
	if (flags & SSTATUS_SIE)
		csr_set(sstatus, SSTATUS_SIE);
	else
		csr_clear(sstatus, SSTATUS_SIE);
}

__always_inline
static inline void local_irq_enable(void)
{
	csr_set(sstatus, SSTATUS_SIE);
}

__always_inline
static inline void local_irq_disable(void)
{
	csr_clear(sstatus, SSTATUS_SIE);
}

__always_inline __must_check
static inline bool irqs_disabled(void)
{
	return (csr_read(sstatus) & SSTATUS_SIE) == 0;
}

#endif

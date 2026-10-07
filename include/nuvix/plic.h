#ifndef _NUVIX_PLIC_H
#define _NUVIX_PLIC_H

struct dtb;

/* Boot-only after pgtable_use_buddy(), before SMP; leaves all sources disabled. */
int plic_init(const struct dtb *dtb);

/* After init; routes only to CPU0. Returns -EINVAL for an invalid source. */
int plic_enable(unsigned int irq);
/* Masks delivery; does not drain handlers or stop device activity. */
int plic_disable(unsigned int irq);

/* CPU0, hard IRQ context with local IRQs off. Zero means no pending source. */
unsigned int plic_claim(void);
/* Complete a nonzero source returned by claim, after handling device state. */
void plic_complete(unsigned int irq);

#endif /* _NUVIX_PLIC_H */

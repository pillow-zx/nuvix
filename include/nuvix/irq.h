#ifndef _NUVIX_IRQ_H
#define _NUVIX_IRQ_H

#include <asm/irq.h>

#define local_irq_disable()      arch_local_irq_disable()
#define local_irq_enable()       arch_local_irq_enable()
#define local_irq_save()         arch_local_irq_save()
#define local_irq_restore(flags) arch_local_irq_restore(flags)
#define local_irqs_disabled()    arch_local_irqs_disabled()

typedef void (*irq_handler_t)(void *data);

struct dtb;

/*
 * Boot-only, once after slub_init(), before SMP, with local IRQs off.
 * Initializes the controller and handler table, leaving all sources disabled.
 * IRQs are numbered from one through the architecture's validated range.
 * The handler table remains allocated for the kernel's lifetime.
 */
int irq_init(const struct dtb *dtb);

/* Boot-only after irq_init(); resolves one device node's interrupt source. */
int irq_parse(const struct dtb *dtb, int node, unsigned int *irq);

/*
 * After irq_init(), before SMP, with local IRQs off; one handler per source.
 * Handler/data must remain alive until unregistered, or for the kernel's lifetime.
 * Does not enable the hardware source.
 * Returns -EINVAL for invalid arguments, -EEXIST for an already bound source.
 */
int irq_register(unsigned int irq, irq_handler_t handler, void *data);

/*
 * Before SMP, with local IRQs off; masks the source and clears handler/data.
 * Does not stop device activity. Returns -EINVAL for an invalid IRQ,
 * -ENOENT for an unbound source, or the controller's disable error.
 */
int irq_unregister(unsigned int irq);

/* After irq_init(); returns -EINVAL for an invalid IRQ. */
int irq_enable(unsigned int irq);
/* Masks delivery; does not drain handlers or stop device activity. */
int irq_disable(unsigned int irq);

/* Hard IRQ context; invokes the handler, which must not sleep. */
void irq_dispatch(unsigned int irq);

#endif /* _NUVIX_IRQ_H */

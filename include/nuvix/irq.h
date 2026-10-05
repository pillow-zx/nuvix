#ifndef _NUVIX_IRQ_H
#define _NUVIX_IRQ_H

#include <asm/irq.h>

#define local_irq_disable()      arch_local_irq_disable()
#define local_irq_enable()       arch_local_irq_enable()
#define local_irq_save()         arch_local_irq_save()
#define local_irq_restore(flags) arch_local_irq_restore(flags)
#define irqs_disabled()          arch_irqs_disabled()

#endif /* _NUVIX_IRQ_H */

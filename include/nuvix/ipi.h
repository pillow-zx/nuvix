#ifndef _NUVIX_IPI_H
#define _NUVIX_IPI_H

/*
 * include/nuvix/ipi.h - generic inter-processor interrupts
 *
 * The current generic IPI is a reschedule notification. Delivery may coalesce;
 * the scheduler state is authoritative, while the interrupt only prompts the
 * target CPU to observe it.
 */

#include <nuvix/types.h>

#ifdef CONFIG_SMP
/* Architecture transport used by the generic IPI implementation. */
int smp_ipi_notify(uint32_t hartid);
void smp_ipi_ack(void);

/*
 * Notify one online CPU. A self-target is handled locally. The scheduler uses
 * this for remote wakeups and the SMP boot-health gate. Returns a negative
 * errno for an invalid/offline target, or the architecture delivery error.
 */
int ipi_send(uint32_t cpu_id);

/* Acknowledge the local software interrupt and request rescheduling. */
void ipi_handle(void);

/* Boot-health observation: true after this CPU handled an IPI once. */
bool ipi_seen(uint32_t cpu_id);

#endif /* CONFIG_SMP */

#endif

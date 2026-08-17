#ifndef _NUVIX_IPI_H
#define _NUVIX_IPI_H

/*
 * include/nuvix/ipi.h - generic inter-processor interrupt reasons
 *
 * Senders release-publish reason bits, then always notify the target hart
 * through the architecture adapter; handlers acknowledge the interrupt,
 * acquire-consume the pending bits, and dispatch. Sends may coalesce.
 */

#include <nuvix/bitops.h>
#include <nuvix/types.h>

#define IPI_RESCHEDULE	 BIT(0)
#define IPI_SHOOTDOWN	 BIT(1)
#define IPI_FENCE_I	 BIT(2)
#define IPI_REASON_MASK	 (IPI_RESCHEDULE | IPI_SHOOTDOWN | IPI_FENCE_I)

/*
 * Deliver one or more reasons to an online, non-self CPU. The scheduler uses
 * this for remote wakeups as well as the boot-health gate. Returns a negative
 * errno for null/self/offline/invalid targets, or the SBI error when
 * notification fails; the reason bits stay pending either way.
 */
int ipi_send(uint32_t cpu_id, int reasons);

/* Run on the current CPU in software-interrupt context: acknowledge the
 * local SSIP, consume pending reasons, and dispatch them. */
void ipi_handle(void);

/* Boot-health observation: true after this CPU handled an IPI once. */
bool ipi_seen(uint32_t cpu_id);

/* Acquire-peek of the pending reasons of one CPU (diagnostics only). */
int ipi_pending_reasons(uint32_t cpu_id);

/* Send shootdown reasons to one online non-self CPU and wait for its
 * ack (1-second mtime deadline, panic on timeout). */
void ipi_send_shootdown(uint32_t cpu_id, int reasons);

#endif

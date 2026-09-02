#ifndef _NUVIX_RSEQ_TYPES_H
#define _NUVIX_RSEQ_TYPES_H

/**
 * @file rseq_types.h
 * @brief Task-embedded restartable sequence storage.
 */

#include <nuvix/atomic.h>
#include <nuvix/types.h>

struct rseq;

/**
 * @struct rseq_task_context
 * @brief Per-task rseq registration cached in task_struct.
 *
 * @par Fields
 * - @c area: Registered userspace rseq area, or NULL.
 * - @c len: Userspace-provided rseq area length.
 * - @c sig: Signature used to validate unregister requests.
 * - @c cpu_id: Logical CPU identity last published to userspace.
 * - @c restart_events: Atomic pending restart-event mask. IPI context may
 *   mark it without taking a task-only lock.
 */
struct rseq_task_context {
	struct rseq *area;
	uint32_t len;
	uint32_t sig;
	uint32_t cpu_id;
	atomic_t restart_events;
};

#endif

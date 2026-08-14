#ifndef _NUVIX_RSEQ_TYPES_H
#define _NUVIX_RSEQ_TYPES_H

/**
 * @file rseq_types.h
 * @brief Task-embedded restartable sequence storage.
 */

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
 * - @c need_update: Userspace cpu_id fields need refresh.
 */
struct rseq_task_context {
	struct rseq *area;
	uint32_t len;
	uint32_t sig;
	uint8_t need_update;
};

#endif

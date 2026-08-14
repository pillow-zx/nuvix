#ifndef _NUVIX_FORK_H
#define _NUVIX_FORK_H

/**
 * @file fork.h
 * @brief clone/fork task creation staging API.
 */

#include <nuvix/types.h>
#include <nuvix/task.h>

struct trap_frame;

/**
 * @struct kernel_clone
 * @brief Prepared clone operation staged between allocation and publication.
 *
 * @par Fields
 * - @c task: Child task under construction.
 * - @c flags: Linux clone flags.
 * - @c pid: Allocated child TID.
 */
struct kernel_clone {
	struct task_struct *task;
	unsigned long flags;
	pid_t pid;
	int *child_tid;
	bool new_proc;
};

/**
 * @brief Allocate and initialize a child task without publishing it.
 * @param tf Parent user trap frame.
 * @param flags Linux clone flags.
 * @param child_stack Optional userspace child stack pointer.
 * @param tls Optional TLS value.
 * @param clear_child_tid Optional userspace futex clear address.
 * @param clone Output staging object.
 * @return 0 on success, or a negative errno.
 */
int kernel_clone_prepare(struct trap_frame *tf, unsigned long flags,
			 uintptr_t child_stack, uintptr_t tls,
			 int *clear_child_tid, struct kernel_clone *clone);

/**
 * @brief Publish a prepared clone as runnable.
 * @param clone Prepared clone object.
 * @return Child TID on success, or a negative errno.
 */
pid_t kernel_clone_commit(struct kernel_clone *clone);

/**
 * @brief Roll back a prepared clone that was not committed.
 * @param clone Prepared clone object.
 */
void kernel_clone_abort(struct kernel_clone *clone);

/**
 * @brief Complete a vfork child after it releases its pre-exec mm.
 * @param task Task that may own an active vfork completion.
 *
 * The operation is idempotent. It has no effect for ordinary clone children.
 */
void kernel_clone_complete_vfork(struct task_struct *task);

#endif

#ifndef _CUTEOS_KERNEL_FUTEX_H
#define _CUTEOS_KERNEL_FUTEX_H

/**
 * @file futex.h
 * @brief Futex wait/wake and robust-list kernel entry points.
 */

#include <kernel/types.h>
#include <kernel/task.h>
#include <uapi/futex.h>

struct wait_deadline;

/**
 * @struct kernel_futex_args
 * @brief Decoded futex syscall arguments passed to the futex module.
 *
 * @par Fields
 * - @c uaddr: Primary userspace futex word.
 * - @c op: Linux futex operation plus option bits.
 * - @c val: Operation-specific integer argument.
 * - @c deadline: Optional absolute mtime deadline.
 * - @c uaddr2: Secondary userspace futex word for future operations.
 * - @c val3: Operation-specific third integer argument.
 */
struct kernel_futex_args {
	int *uaddr;
	int op;
	int val;
	const struct wait_deadline *deadline;
	int *uaddr2;
	int val3;
};

__must_check __pure
static inline int *
task_clear_child_tid(struct task_struct *task)
{
	return task ? task->sigctx.clear_child_tid : NULL;
}

static inline void task_set_clear_child_tid(struct task_struct *task,
						     int *uaddr)
{
	if (task)
		task->sigctx.clear_child_tid = uaddr;
}

__must_check __pure
static inline struct robust_list_head *
task_robust_list(struct task_struct *task)
{
	return task ? task->sigctx.robust_list : NULL;
}

__must_check __pure
static inline size_t
task_robust_list_len(struct task_struct *task)
{
	return task ? task->sigctx.robust_list_len : 0;
}

static inline void task_set_robust_list(struct task_struct *task,
						 struct robust_list_head *head,
						 size_t len)
{
	if (!task)
		return;
	task->sigctx.robust_list = head;
	task->sigctx.robust_list_len = len;
}

void futex_init(void);

/**
 * @brief Wake tasks waiting on a futex word in one address space.
 * @param mm Address space containing @p uaddr.
 * @param uaddr Userspace futex word.
 * @param nr Maximum number of waiters to wake.
 * @return Number of tasks woken.
 */
__must_check __nonnull(1, 2)
int futex_wake_mm(struct mm_struct *mm, int *uaddr, int nr);

/**
 * @brief Process a task's robust futex list during exit.
 * @param task Exiting task.
 */
void futex_exit_robust_list(struct task_struct *task);

/**
 * @brief Implement supported Linux futex operations for the current task.
 * @param args Decoded futex syscall arguments.
 * @return Operation result or a negative errno.
 */
__must_check __nonnull(1) __access_no_size(read_only, 1)
int kernel_futex(const struct kernel_futex_args *args);

__must_check
int futex_set_robust_list(struct task_struct *task,
			  struct robust_list_head *head, size_t len);

__must_check __nonnull(1, 2, 3)
__access_no_size(write_only, 2) __access_no_size(write_only, 3)
int futex_get_robust_list(struct task_struct *task,
			  struct robust_list_head **head, size_t *len);

#endif

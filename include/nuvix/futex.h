#ifndef _NUVIX_FUTEX_H
#define _NUVIX_FUTEX_H

/**
 * @file futex.h
 * @brief Futex wait/wake and robust-list kernel entry points.
 */

#include <nuvix/types.h>
#include <nuvix/cleanup.h>
#include <nuvix/mm.h>
#include <nuvix/task.h>
#include <uapi/futex.h>

struct wait_deadline;

/** Owned identity retained until the corresponding futex wake completes. */
struct futex_key {
	bool shared;
	union {
		struct {
			struct mm_struct *mm;
			uintptr_t uaddr;
		} priv;
		struct mm_map_id shared_file;
	};
};

/**
 * @struct futex_args
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
struct futex_args {
	int *uaddr;
	int op;
	int val;
	const struct wait_deadline *deadline;
	int *uaddr2;
	int val3;
};

static inline void task_set_clear_child_tid(struct task_struct *task,
						     int *uaddr)
{
	if (task)
		task->signal.clear_child_tid = uaddr;
}

static inline void task_set_robust_list(struct task_struct *task,
							 struct robust_list_head *head,
						 size_t len)
{
	if (!task)
		return;
	task->signal.robust_list = head;
	task->signal.robust_list_len = len;
}

void futex_init(void);

/** Construct a key while the caller already holds mm->mmap_lock. */
__must_check __nonnull(1)
int futex_key_init_locked(struct futex_key *key, struct mm_struct *mm,
				  uintptr_t uaddr);

/** Construct a key and acquire the required mapping lifetime. */
__must_check __nonnull(1)
int futex_key_init(struct futex_key *key, struct mm_struct *mm,
			   uintptr_t uaddr);

void futex_key_put(struct futex_key *key);

CLEANUP_DEFINE(futex_key_ref, struct futex_key, futex_key_put(&_T));

/** Wake waiters using an already-owned key. */
__must_check __nonnull(1)
int futex_wake_key(const struct futex_key *key, int nr);

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
int futex(const struct futex_args *args);

__must_check
int futex_set_robust_list(struct task_struct *task,
			  struct robust_list_head *head, size_t len);

#endif

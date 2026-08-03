#ifndef _CUTEOS_KERNEL_TASK_INTERNAL_H
#define _CUTEOS_KERNEL_TASK_INTERNAL_H

#include <kernel/task.h>

/* Fork/exec-only user-process role transitions. */
void task_inherit_process_role(struct task_struct *child,
			       const struct task_struct *parent);
void task_mark_user_process(struct task_struct *task);

/* Session-coordinator-only process identity mutations. */
int task_process_clone_identity(struct task_struct *child,
				const struct task_struct *parent) __must_check;
int task_process_setsid(struct task_struct *task,
			struct task_process_identity *previous) __must_check;
int task_process_setpgid(struct task_struct *caller, pid_t pid, pid_t pgid,
			 struct task_process_identity *previous) __must_check;

#endif

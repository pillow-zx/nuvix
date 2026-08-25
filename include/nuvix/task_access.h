#ifndef _NUVIX_TASK_ACCESS_H
#define _NUVIX_TASK_ACCESS_H

#include <nuvix/cleanup.h>
#include <nuvix/compiler.h>

/** Named policy boundary for cross-Task inspection and mutation. */
enum task_access_mode {
	TASK_ACCESS_SCHEDULER_WRITE,
};

struct cred;
struct task_struct;

void cred_put(struct cred *cred);

/** Take a protected reference to an immutable credential snapshot. */
__must_check
struct cred *task_cred_get(struct task_struct *task);

CLEANUP_DEFINE(cred_ref, struct cred *, if (_T) cred_put(_T));

/** Check @caller access to @target under one named Task access mode. */
__must_check
int task_access_check(struct task_struct *caller, struct task_struct *target,
			      enum task_access_mode mode);

#endif

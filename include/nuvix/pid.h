/**
 * @file pid.h
 * @brief PID identities, role attachment, allocation, and lookup.
 *
 * Locking: all registry state is guarded by the module-private pid_lock
 * (LOCK_RANK_PID, 70).  Callers that also hold the topology lock (60) may
 * publish/unpublish; nothing else nests under pid_lock.  Lookups
 * return an object with one strong reference; the caller owns it.
 *
 * Reference rules: the registry reference is acquired once at first
 * publication and released when the last role detaches.  pid_unpublish_proc()
 * reports that release so the caller can run the matching pid_put() outside
 * any lock it holds.  pid_put() itself never runs under pid_lock.
 */

#ifndef _NUVIX_PID_H
#define _NUVIX_PID_H

#include <nuvix/refcount.h>
#include <nuvix/types.h>

struct pgrp_struct;
struct proc_struct;
struct session_struct;
struct task_struct;

/**
 * @struct pid_identity
 * @brief One numerical identity carrying independently attached PID roles.
 */
struct pid_identity {
	refcount_t refs;
	pid_t nr;
	bool published;
	struct task_struct *task;
	struct proc_struct *proc;
	struct pgrp_struct *pgrp;
	struct session_struct *session;
};

/** Highest allocatable PID/TID in the single PID namespace. */
#define PID_MAX		255
/** Number of identity slots, including the reserved zero slot. */
#define PID_COUNT	256

void pid_init(void);

struct pid_identity *pid_alloc(void) __must_check;
void pid_get(struct pid_identity *identity);
void pid_put(struct pid_identity *identity);
bool pid_try_get(struct pid_identity *identity) __must_check;

int pid_publish_task(struct pid_identity *identity, struct task_struct *task);
int pid_publish_proc(struct pid_identity *identity, struct proc_struct *proc);
int pid_publish_task_proc(struct pid_identity *identity,
			  struct task_struct *task,
			  struct proc_struct *proc);
void pid_unpublish_task(struct pid_identity *identity, struct task_struct *task);

/*
 * Unpublish the proc role.  Returns true when the identity lost its last
 * role and the registry reference must be released; the caller performs
 * the pid_put() outside any held lock (a final put destroys the identity).
 */
bool pid_unpublish_proc(struct pid_identity *identity, struct proc_struct *proc);

/**
 * Transfer an already-published task role between identities.
 *
 * The source identity must own @p task and the destination must have no task
 * role.  The caller transfers its task-owned identity reference after this
 * operation succeeds.
 */
int pid_transfer_task_role(struct pid_identity *from,
				   struct pid_identity *to,
				   struct task_struct *task,
				   bool *drop_registry);

struct task_struct *pid_lookup_task(pid_t nr) __must_check;
struct proc_struct *pid_lookup_proc(pid_t nr) __must_check;
struct pid_identity *pid_lookup_identity(pid_t nr) __must_check;
struct pgrp_struct *pid_lookup_pgrp(pid_t nr) __must_check;
struct session_struct *pid_lookup_session(pid_t nr) __must_check;

int pid_attach_pgrp(struct pid_identity *identity, struct pgrp_struct *pgrp);
void pid_detach_pgrp(struct pid_identity *identity, struct pgrp_struct *pgrp);
int pid_attach_session(struct pid_identity *identity,
			       struct session_struct *session);
void pid_detach_session(struct pid_identity *identity,
				struct session_struct *session);

/* Kept private to the PID implementation while callers are migrated. */
int32_t alloc_pid(void);
void free_pid(pid_t pid);
uint16_t pid_count_tasks(void);

#endif

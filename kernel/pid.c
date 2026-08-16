/*
 * kernel/pid.c - single-namespace PID identity registry
 */

#include <nuvix/errno.h>
#include <nuvix/pid.h>
#include <nuvix/proc.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/task.h>

static DEFINE_SPINLOCK(pid_lock, LOCK_RANK_PID, LOCK_IRQ_TASK_ONLY);
static bool pid_used[PID_COUNT];
static struct pid_identity *pid_table[PID_COUNT];

static bool pid_nr_valid(pid_t nr)
{
	return nr >= 0 && nr <= PID_MAX;
}

static void pid_identity_destroy(struct pid_identity *identity)
{
	BUG_ON(!identity);
	BUG_ON(identity->published);
	BUG_ON(identity->task || identity->proc || identity->pgrp ||
	       identity->session);
	BUG_ON(pid_used[identity->nr]);
	kfree(identity);
}

void pid_init(void)
{
	spin_lock(&pid_lock);
	memset(pid_used, 0, sizeof(pid_used));
	memset(pid_table, 0, sizeof(pid_table));
	pid_used[0] = true;
	spin_unlock(&pid_lock);
}

struct pid_identity *pid_alloc(void)
{
	struct pid_identity *identity;

	identity = kzalloc(sizeof(*identity), ALLOC_NOWAIT);
	if (!identity)
		return NULL;

	spin_lock(&pid_lock);
	for (pid_t nr = 1; nr <= PID_MAX; nr++) {
		if (pid_used[nr])
			continue;
		pid_used[nr] = true;
		identity->nr = nr;
		refcount_set(&identity->refs, 1);
		spin_unlock(&pid_lock);
		return identity;
	}
	spin_unlock(&pid_lock);
	kfree(identity);
	return NULL;
}

void pid_get(struct pid_identity *identity)
{
	if (identity)
		refcount_inc(&identity->refs);
}

bool pid_try_get(struct pid_identity *identity)
{
	return identity && refcount_inc_not_zero(&identity->refs);
}

void pid_put(struct pid_identity *identity)
{
	bool destroy;

	if (!identity)
		return;
	destroy = refcount_dec_and_test(&identity->refs);
	if (!destroy)
		return;

	spin_lock(&pid_lock);
	BUG_ON(identity->published);
	BUG_ON(identity->task || identity->proc || identity->pgrp ||
	       identity->session);
	BUG_ON(!pid_used[identity->nr]);
	pid_used[identity->nr] = false;
	spin_unlock(&pid_lock);
	pid_identity_destroy(identity);
}

static int pid_publish_role(struct pid_identity *identity, void *owner,
				 void **slot)
{
	if (!identity || !owner || !slot)
		return -EINVAL;
	spin_lock(&pid_lock);
	if (*slot) {
		spin_unlock(&pid_lock);
		return -EBUSY;
	}
	*slot = owner;
	if (!identity->published) {
		identity->published = true;
		pid_table[identity->nr] = identity;
		refcount_inc(&identity->refs); /* registry reference */
	}
	spin_unlock(&pid_lock);
	return 0;
}

int pid_publish_task(struct pid_identity *identity, struct task_struct *task)
{
	return pid_publish_role(identity, task, (void **)&identity->task);
}

int pid_publish_proc(struct pid_identity *identity, struct proc_struct *proc)
{
	return pid_publish_role(identity, proc, (void **)&identity->proc);
}

int pid_publish_task_proc(struct pid_identity *identity,
			  struct task_struct *task,
			  struct proc_struct *proc)
{
	if (!identity || !task || !proc)
		return -EINVAL;
	spin_lock(&pid_lock);
	if (identity->task || identity->proc) {
		spin_unlock(&pid_lock);
		return -EBUSY;
	}
	identity->task = task;
	identity->proc = proc;
	if (!identity->published) {
		identity->published = true;
		pid_table[identity->nr] = identity;
		refcount_inc(&identity->refs); /* registry reference */
	}
	spin_unlock(&pid_lock);
	return 0;
}

static bool pid_unpublish_role(struct pid_identity *identity, void *owner,
			       void **slot)
{
	bool drop_registry = false;

	if (!identity || !owner || !slot)
		return false;
	spin_lock(&pid_lock);
	if (*slot == owner)
		*slot = NULL;
	if (!identity->task && !identity->proc && !identity->pgrp &&
	    !identity->session && identity->published) {
		identity->published = false;
		BUG_ON(pid_table[identity->nr] != identity);
		pid_table[identity->nr] = NULL;
		drop_registry = true;
	}
	spin_unlock(&pid_lock);
	return drop_registry;
}

void pid_unpublish_task(struct pid_identity *identity, struct task_struct *task)
{
	if (pid_unpublish_role(identity, task, (void **)&identity->task))
		pid_put(identity);
}

/*
 * Returns true when this unpublish dropped the last role and released the
 * registry reference.  The caller must run the matching pid_put() outside
 * any lock it holds: a final put destroys the identity.
 */
bool pid_unpublish_proc(struct pid_identity *identity, struct proc_struct *proc)
{
	return pid_unpublish_role(identity, proc, (void **)&identity->proc);
}

int pid_transfer_task_role(struct pid_identity *from,
				   struct pid_identity *to,
				   struct task_struct *task,
				   bool *drop_registry)
{
	if (!from || !to || !task || !drop_registry)
		return -EINVAL;
	*drop_registry = false;
	if (from == to)
		return 0;

	spin_lock(&pid_lock);
	if (from->task != task || to->task) {
		spin_unlock(&pid_lock);
		return -EBUSY;
	}

	/* The task retains its old identity reference until the caller updates it. */
	to->task = task;
	pid_get(to);
	if (!to->published) {
		BUG_ON(pid_table[to->nr]);
		to->published = true;
		pid_table[to->nr] = to;
		refcount_inc(&to->refs); /* registry reference */
	}

	from->task = NULL;
	if (!from->proc && !from->pgrp && !from->session && from->published) {
		BUG_ON(pid_table[from->nr] != from);
		from->published = false;
		pid_table[from->nr] = NULL;
		*drop_registry = true;
	}
	spin_unlock(&pid_lock);
	return 0;
}

struct task_struct *pid_lookup_task(pid_t nr)
{
	struct task_struct *task = NULL;
	struct pid_identity *identity;

	if (!pid_nr_valid(nr))
		return NULL;
	spin_lock(&pid_lock);
	identity = pid_table[nr];
	if (identity && identity->task && task_try_get_live(identity->task))
		task = identity->task;
	spin_unlock(&pid_lock);
	return task;
}

struct proc_struct *pid_lookup_proc(pid_t nr)
{
	struct proc_struct *proc = NULL;
	struct pid_identity *identity;

	if (!pid_nr_valid(nr))
		return NULL;
	spin_lock(&pid_lock);
	identity = pid_table[nr];
	if (identity && identity->proc && proc_try_get(identity->proc))
		proc = identity->proc;
	spin_unlock(&pid_lock);
	return proc;
}

struct pid_identity *pid_lookup_identity(pid_t nr)
{
	struct pid_identity *identity = NULL;

	if (!pid_nr_valid(nr))
		return NULL;
	spin_lock(&pid_lock);
	identity = pid_table[nr];
	if (!identity || !pid_try_get(identity))
		identity = NULL;
	spin_unlock(&pid_lock);
	return identity;
}

struct pgrp_struct *pid_lookup_pgrp(pid_t nr)
{
	struct pgrp_struct *pgrp = NULL;
	struct pid_identity *identity;

	if (!pid_nr_valid(nr))
		return NULL;
	spin_lock(&pid_lock);
	identity = pid_table[nr];
	if (identity && identity->pgrp) {
		pgrp = identity->pgrp;
		if (!proc_pgrp_try_get(pgrp))
			pgrp = NULL;
	}
	spin_unlock(&pid_lock);
	return pgrp;
}

struct session_struct *pid_lookup_session(pid_t nr)
{
	struct session_struct *session = NULL;
	struct pid_identity *identity;

	if (!pid_nr_valid(nr))
		return NULL;
	spin_lock(&pid_lock);
	identity = pid_table[nr];
	if (identity && identity->session) {
		session = identity->session;
		if (!proc_session_try_get(session))
			session = NULL;
	}
	spin_unlock(&pid_lock);
	return session;
}

static int pid_attach_role(struct pid_identity *identity, void *owner,
				   void **slot)
{
	if (!identity || !owner || !slot)
		return -EINVAL;
	spin_lock(&pid_lock);
	if (*slot && *slot != owner) {
		spin_unlock(&pid_lock);
		return -EBUSY;
	}
	if (!*slot) {
		*slot = owner;
		pid_get(identity);
	}
	spin_unlock(&pid_lock);
	return 0;
}

static void pid_detach_role(struct pid_identity *identity, void *owner,
				    void **slot)
{
	bool drop_role = false;
	bool drop_registry = false;

	if (!identity || !owner || !slot)
		return;
	spin_lock(&pid_lock);
	if (*slot == owner) {
		*slot = NULL;
		drop_role = true;
	}
	if (drop_role && !identity->task && !identity->proc &&
	    !identity->pgrp && !identity->session && identity->published) {
		BUG_ON(pid_table[identity->nr] != identity);
		identity->published = false;
		pid_table[identity->nr] = NULL;
		drop_registry = true;
	}
	spin_unlock(&pid_lock);
	if (drop_registry)
		pid_put(identity);
	if (drop_role)
		pid_put(identity);
}

int pid_attach_pgrp(struct pid_identity *identity, struct pgrp_struct *pgrp)
{
	return pid_attach_role(identity, pgrp, (void **)&identity->pgrp);
}

void pid_detach_pgrp(struct pid_identity *identity, struct pgrp_struct *pgrp)
{
	pid_detach_role(identity, pgrp, (void **)&identity->pgrp);
}

int pid_attach_session(struct pid_identity *identity,
			       struct session_struct *session)
{
	return pid_attach_role(identity, session, (void **)&identity->session);
}

void pid_detach_session(struct pid_identity *identity,
				struct session_struct *session)
{
	pid_detach_role(identity, session, (void **)&identity->session);
}

/* Temporary counters retained only for the existing sysinfo implementation. */
int32_t alloc_pid(void)
{
	struct pid_identity *identity = pid_alloc();
	int32_t nr;

	if (!identity)
		return -EAGAIN;
	nr = identity->nr;
	pid_put(identity);
	return nr;
}

void free_pid(pid_t pid)
{
	struct pid_identity *identity;

	if (!pid_nr_valid(pid) || pid == 0)
		return;
	spin_lock(&pid_lock);
	identity = pid_table[pid];
	if (identity || !pid_used[pid]) {
		spin_unlock(&pid_lock);
		return;
	}
	pid_used[pid] = false;
	spin_unlock(&pid_lock);
}

uint16_t pid_count_tasks(void)
{
	uint16_t count = 0;

	spin_lock(&pid_lock);
	for (pid_t nr = 1; nr <= PID_MAX; nr++)
		if (pid_table[nr] && pid_table[nr]->task)
			count++;
	spin_unlock(&pid_lock);
	return count;
}

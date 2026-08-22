/*
 * kernel/proc.c - process object, topology, and wait-visible lifecycle
 */

#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs_struct.h>
#include <nuvix/init.h>
#include <nuvix/mm.h>
#include <nuvix/pid.h>
#include <nuvix/proc.h>
#include <nuvix/resource.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <uapi/signal.h>

static DEFINE_SPINLOCK(proc_topology_lock, LOCK_RANK_TOPOLOGY,
		       LOCK_IRQ_TASK_ONLY);

void proc_parent_event_release(struct proc_parent_event *event)
{
	if (!event)
		return;
	proc_put(event->child);
	proc_put(event->parent);
	memset(event, 0, sizeof(*event));
}

void proc_orphan_event_release(struct proc_orphan_event *event)
{
	if (!event)
		return;
	proc_pgrp_put(event->pgrp);
	proc_session_put(event->session);
	memset(event, 0, sizeof(*event));
}

void proc_pgrp_get(struct pgrp_struct *pgrp)
{
	if (pgrp)
		refcount_inc(&pgrp->refs);
}

bool proc_pgrp_try_get(struct pgrp_struct *pgrp)
{
	return pgrp && refcount_inc_not_zero(&pgrp->refs);
}

void proc_session_get(struct session_struct *session)
{
	if (session)
		refcount_inc(&session->refs);
}

struct session_struct *proc_session_ref_of(const struct proc_struct *proc)
{
	struct session_struct *session;

	if (!proc || !proc->pgrp)
		return NULL;
	spin_lock(&proc_topology_lock);
	session = proc->pgrp->session;
	if (session)
		proc_session_get(session);
	spin_unlock(&proc_topology_lock);
	return session;
}

bool proc_session_try_get(struct session_struct *session)
{
	return session && refcount_inc_not_zero(&session->refs);
}

void proc_pgrp_put(struct pgrp_struct *pgrp)
{
	struct session_struct *session;
	struct pid_identity *identity;

	if (!pgrp || !refcount_dec_and_test(&pgrp->refs))
		return;

	spin_lock(&proc_topology_lock);
	BUG_ON(!list_empty(&pgrp->members));
	session = pgrp->session;
	identity = pgrp->pgid;
	if (session && !list_empty(&pgrp->session_node))
		list_del_init(&pgrp->session_node);
	pgrp->session = NULL;
	pgrp->pgid = NULL;
	spin_unlock(&proc_topology_lock);
	if (identity)
		pid_detach_pgrp(identity, pgrp);
	if (identity)
		pid_put(identity);
	kfree(pgrp);
	proc_session_put(session);
}

void proc_session_put(struct session_struct *session)
{
	struct pid_identity *identity;

	if (!session || !refcount_dec_and_test(&session->refs))
		return;

	spin_lock(&proc_topology_lock);
	BUG_ON(!list_empty(&session->pgrps));
	identity = session->sid;
	session->sid = NULL;
	spin_unlock(&proc_topology_lock);
	if (identity)
		pid_detach_session(identity, session);
	if (identity)
		pid_put(identity);
	kfree(session);
}

static struct session_struct *session_alloc(struct pid_identity *sid)
{
	struct session_struct *session;

	session = kzalloc(sizeof(*session), ALLOC_NOWAIT);
	if (!session)
		return NULL;
	refcount_set(&session->refs, 1);
	spin_lock_init(&session->lock, LOCK_RANK_TOPOLOGY, LOCK_IRQ_TASK_ONLY);
	INIT_LIST_HEAD(&session->pgrps);
	session->ctty = NULL;
	session->sid = sid;
	pid_get(sid);
	if (pid_attach_session(sid, session) < 0) {
		pid_put(sid);
		kfree(session);
		return NULL;
	}
	return session;
}

static struct pgrp_struct *pgrp_alloc(struct pid_identity *pgid,
				      struct session_struct *session)
{
	struct pgrp_struct *pgrp;

	pgrp = kzalloc(sizeof(*pgrp), ALLOC_NOWAIT);
	if (!pgrp)
		return NULL;
	refcount_set(&pgrp->refs, 1);
	spin_lock_init(&pgrp->lock, LOCK_RANK_TOPOLOGY, LOCK_IRQ_TASK_ONLY);
	INIT_LIST_HEAD(&pgrp->members);
	INIT_LIST_HEAD(&pgrp->session_node);
	pgrp->orphaned = true;
	pgrp->pgid = pgid;
	pgrp->session = session;
	pid_get(pgid);
	proc_session_get(session);
	if (pid_attach_pgrp(pgid, pgrp) < 0) {
		pid_put(pgid);
		proc_session_put(session);
		kfree(pgrp);
		return NULL;
	}
	spin_lock(&proc_topology_lock);
	list_add_tail(&pgrp->session_node, &session->pgrps);
	spin_unlock(&proc_topology_lock);
	return pgrp;
}

static bool proc_pgrp_is_orphaned_locked(const struct pgrp_struct *pgrp)
{
	const struct proc_struct *member;

	list_for_each_entry (member, &pgrp->members, pgrp_node) {
		const struct proc_struct *parent = member->parent;

		if (parent && parent->pgrp &&
		    parent->pgrp->session == pgrp->session &&
		    parent->pgrp != pgrp)
			return false;
	}
	return true;
}

static bool proc_pgrp_has_stopped_member_locked(const struct pgrp_struct *pgrp)
{
	const struct proc_struct *member;

	list_for_each_entry (member, &pgrp->members, pgrp_node)
		if (member->job_stopped)
			return true;
	return false;
}

static void proc_orphan_event_add_locked(struct pgrp_struct *pgrp,
					 struct proc_orphan_event *events,
					 size_t capacity, size_t *count)
{
	pid_t pgid;
	pid_t sid;

	if (!events || !count || *count >= capacity || !pgrp || !pgrp->pgid ||
	    !pgrp->session || !pgrp->session->sid)
		return;
	pgid = pgrp->pgid->nr;
	sid = pgrp->session->sid->nr;
	for (size_t index = 0; index < *count; index++)
		if (events[index].pgid == pgid && events[index].sid == sid)
			return;
	proc_pgrp_get(pgrp);
	proc_session_get(pgrp->session);
	events[(*count)++] = (struct proc_orphan_event){
		.pgid = pgid,
		.sid = sid,
		.pgrp = pgrp,
		.session = pgrp->session,
	};
}

static void proc_update_pgrp_orphaned_locked(struct pgrp_struct *pgrp,
					     struct proc_orphan_event *events,
					     size_t capacity, size_t *count)
{
	bool orphaned;

	if (!pgrp)
		return;
	orphaned = proc_pgrp_is_orphaned_locked(pgrp);
	if (!pgrp->orphaned && orphaned &&
	    proc_pgrp_has_stopped_member_locked(pgrp))
		proc_orphan_event_add_locked(pgrp, events, capacity, count);
	pgrp->orphaned = orphaned;
}

static void proc_wait_init(struct proc_wait_state *wait)
{
	spin_lock_init(&wait->lock, LOCK_RANK_WAIT, LOCK_IRQ_HARDIRQ_REACHABLE);
	wait_channel_init(&wait->channel);
	wait->generation = 0;
	wait->exit_generation = 0;
	wait->stop_generation = 0;
	wait->continue_generation = 0;
	wait->claimed_exit_generation = 0;
	wait->claimed_stop_generation = 0;
	wait->claimed_continue_generation = 0;
	wait->pending = 0;
	wait->exit_status = 0;
	wait->stop_status = 0;
	wait->continue_status = 0;
	wait->exit_signal = SIGCHLD;
	wait->creator_tid = 0;
}

static void proc_vfork_init(struct proc_vfork_state *vfork)
{
	spin_lock_init(&vfork->lock, LOCK_RANK_VFORK, LOCK_IRQ_TASK_ONLY);
	wait_channel_init(&vfork->channel);
	vfork->active = false;
	vfork->completed = false;
}

struct proc_struct *proc_alloc(struct proc_struct *parent,
			       struct pid_identity *pid)
{
	struct proc_struct *proc;
	struct session_struct *session;
	struct pgrp_struct *pgrp;

	if (!pid)
		return NULL;
	proc = kzalloc(sizeof(*proc), ALLOC_NOWAIT);
	if (!proc)
		return NULL;
	refcount_set(&proc->refs, 1);
	spin_lock_init(&proc->lock, LOCK_RANK_PROC, LOCK_IRQ_TASK_ONLY);
	proc->pid = pid;
	pid_get(pid);
	proc->lifecycle = PROC_NEW;
	INIT_LIST_HEAD(&proc->tasks);
	INIT_LIST_HEAD(&proc->children);
	INIT_LIST_HEAD(&proc->sibling);
	INIT_LIST_HEAD(&proc->pgrp_node);
	proc_wait_init(&proc->wait_state);
	proc_vfork_init(&proc->vfork);
	wait_channel_init(&proc->exec_channel);
	proc->exec_owner = NULL;
	proc->parent = NULL;
	proc->leader = NULL;
	proc->group_exit_status = 0;
	proc->group_exit_status_valid = false;
	proc->group_exit_requested = false;
	proc->exit_status = 0;
	proc->exit_uid = 0;
	proc->exit_pending = false;
	proc->exit_auto_reap = false;
	proc->exit_sigchld_notify = false;
	proc->user_process = false;
	proc->job_stopped = false;

	if (parent) {
		if (!proc_try_get(parent))
			goto fail;
		proc->parent = parent;
		spin_lock(&proc_topology_lock);
		if (!parent->pgrp || parent->lifecycle >= PROC_EXITING) {
			spin_unlock(&proc_topology_lock);
			proc_put(parent);
			proc->parent = NULL;
			goto fail;
		}
		proc->pgrp = parent->pgrp;
		proc_pgrp_get(proc->pgrp);
		list_add_tail(&proc->pgrp_node, &proc->pgrp->members);
		spin_unlock(&proc_topology_lock);
		return proc;
	}

	session = session_alloc(pid);
	if (!session)
		goto fail;
	pgrp = pgrp_alloc(pid, session);
	if (!pgrp) {
		proc_session_put(session);
		goto fail;
	}
	proc->pgrp = pgrp;
	list_add_tail(&proc->pgrp_node, &pgrp->members);
	/* The pgrp retains the session. The initial session ref is released. */
	proc_session_put(session);
	return proc;

fail:
	if (proc->pgrp) {
		proc_pgrp_put(proc->pgrp);
		proc->pgrp = NULL;
	}
	if (proc->parent)
		proc_put(proc->parent);
	pid_put(proc->pid);
	kfree(proc);
	return NULL;
}

void proc_get(struct proc_struct *proc)
{
	if (proc)
		refcount_inc(&proc->refs);
}

bool proc_try_get(struct proc_struct *proc)
{
	return proc && refcount_inc_not_zero(&proc->refs);
}

static void proc_destroy(struct proc_struct *proc)
{
	BUG_ON(!proc);
	BUG_ON(proc->published);
	BUG_ON(proc->lifecycle != PROC_DEAD && proc->lifecycle != PROC_NEW);
	BUG_ON(proc->nr_tasks != 0);
	BUG_ON(proc->exec_owner);
	BUG_ON(!list_empty(&proc->children));
	BUG_ON(!list_empty(&proc->sibling));
	proc_release_resources(proc);
	if (proc->pgrp) {
		struct pgrp_struct *pgrp = proc->pgrp;

		spin_lock(&proc_topology_lock);
		if (!list_empty(&proc->pgrp_node)) {
			list_del_init(&proc->pgrp_node);
			proc_update_pgrp_orphaned_locked(pgrp, NULL, 0, NULL);
		}
		spin_unlock(&proc_topology_lock);
		proc_pgrp_put(pgrp);
		proc->pgrp = NULL;
	}
	if (proc->parent)
		proc_put(proc->parent);
	if (proc->pid)
		pid_put(proc->pid);
	kfree(proc);
}

void proc_put(struct proc_struct *proc)
{
	if (proc && refcount_dec_and_test(&proc->refs))
		proc_destroy(proc);
}

int proc_init_resources(struct proc_struct *proc)
{
	struct files_struct *files;
	struct fs_struct *fs;
	struct files_struct *old_files;
	struct fs_struct *old_fs;

	if (!proc)
		return -EINVAL;
	files = files_alloc();
	if (!files)
		return -ENOMEM;
	files_install_standard_fds(files);
	fs = fs_alloc();
	if (!fs)
		goto fail_files;
	if (sig_proc_init(proc) < 0)
		goto fail_fs;
	old_files = proc_replace_files(proc, files);
	old_fs = proc_replace_fs(proc, fs);
	files_put(old_files);
	fs_put(old_fs);
	rlimits_init(proc->rlimits);
	return 0;

fail_fs:
	fs_put(fs);
fail_files:
	files_put(files);
	return -ENOMEM;
}

int proc_clone_rlimits(struct proc_struct *proc,
		       struct proc_struct *source)
{
	irq_flags_t flags;

	if (!proc || !source || proc->lifecycle != PROC_NEW)
		return -EINVAL;
	spin_lock_irqsave(&source->lock, &flags);
	memcpy(proc->rlimits, source->rlimits, sizeof(proc->rlimits));
	spin_unlock_irqrestore(&source->lock, flags);
	return 0;
}

void proc_release_resources(struct proc_struct *proc)
{
	struct mm_struct *mm;
	struct files_struct *files;
	struct fs_struct *fs;

	if (!proc)
		return;
	mm = proc_replace_mm(proc, NULL);
	files = proc_replace_files(proc, NULL);
	fs = proc_replace_fs(proc, NULL);
	mm_put(mm);
	files_put(files);
	fs_put(fs);
	sig_proc_release(proc);
}

struct mm_struct *proc_replace_mm(struct proc_struct *proc,
				  struct mm_struct *mm)
{
	struct mm_struct *oldmm;

	if (!proc)
		return NULL;
	spin_lock(&proc->lock);
	oldmm = proc->mm;
	proc->mm = mm;
	spin_unlock(&proc->lock);
	return oldmm;
}

struct files_struct *proc_replace_files(struct proc_struct *proc,
					struct files_struct *files)
{
	struct files_struct *old_files;

	if (!proc)
		return NULL;
	spin_lock(&proc->lock);
	old_files = proc->files;
	proc->files = files;
	spin_unlock(&proc->lock);
	return old_files;
}

struct fs_struct *proc_replace_fs(struct proc_struct *proc,
					struct fs_struct *fs)
{
	struct fs_struct *old_fs;

	if (!proc)
		return NULL;
	spin_lock(&proc->lock);
	old_fs = proc->fs;
	proc->fs = fs;
	spin_unlock(&proc->lock);
	return old_fs;
}

int proc_publish(struct proc_struct *proc)
{
	int ret;

	if (!proc || proc->lifecycle != PROC_NEW || !proc->leader)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	ret = pid_publish_proc(proc->pid, proc);
	if (ret == 0) {
		proc_get(proc); /* publication reference lasts until unpublish */
		proc->published = true;
		proc->lifecycle = PROC_LIVE;
	}
	spin_unlock(&proc_topology_lock);
	return ret;
}

int proc_publish_with_task(struct proc_struct *proc, struct task_struct *task)
{
	int ret;

	if (!proc || !task || proc->lifecycle != PROC_NEW ||
	    proc->leader != task || task->proc != proc ||
	    proc->pid != task->tid)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	proc->lifecycle = PROC_LIVE;
	ret = pid_publish_task_proc(proc->pid, task, proc);
	if (ret == 0) {
		proc_get(proc); /* publication reference lasts until unpublish */
		proc->published = true;
	} else {
		proc->lifecycle = PROC_NEW;
	}
	spin_unlock(&proc_topology_lock);
	return ret;
}

void proc_unpublish(struct proc_struct *proc)
{
	bool unpublished = false;
	bool drop_registry = false;

	if (!proc || !proc->published)
		return;
	spin_lock(&proc_topology_lock);
	if (proc->published) {
		proc->published = false;
		drop_registry = pid_unpublish_proc(proc->pid, proc);
		unpublished = true;
	}
	spin_unlock(&proc_topology_lock);
	if (unpublished) {
		if (drop_registry)
			pid_put(proc->pid); /* drop the deferred registry reference */
		proc_put(proc);		    /* drop publication reference */
	}
}

void proc_account_task_cputime(struct proc_struct *proc,
			       const struct task_cputime *time)
{
	if (!proc || !time)
		return;
	spin_lock(&proc->lock);
	cputime_add(&proc->cputime, time);
	spin_unlock(&proc->lock);
}

void proc_account_child_cputime(struct proc_struct *proc,
				const struct task_cputime *time)
{
	if (!proc || !time)
		return;
	spin_lock(&proc->lock);
	cputime_add(&proc->children_cputime, time);
	spin_unlock(&proc->lock);
}

void proc_cputime_snapshot(struct proc_struct *proc,
			   struct proc_cputime_snapshot *snapshot)
{
	if (!snapshot)
		return;
	memset(snapshot, 0, sizeof(*snapshot));
	if (!proc)
		return;
	spin_lock(&proc->lock);
	snapshot->self = proc->cputime;
	snapshot->children = proc->children_cputime;
	spin_unlock(&proc->lock);
}

int proc_attach_task(struct proc_struct *proc, struct task_struct *task,
		     bool leader)
{
	if (!proc || !task || task->proc)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	if (proc->lifecycle >= PROC_EXITING || proc->exec_owner) {
		spin_unlock(&proc_topology_lock);
		return -ESRCH;
	}
	proc_get(proc);
	task->proc = proc;
	list_add_tail(&task->proc_node, &proc->tasks);
	proc->nr_tasks++;
	if (leader || !proc->leader)
		proc->leader = task;
	spin_unlock(&proc_topology_lock);
	return 0;
}

bool proc_is_last_task(const struct proc_struct *proc,
		       const struct task_struct *task)
{
	bool last = false;

	if (!proc || !task)
		return false;
	spin_lock((spinlock_t *)&proc_topology_lock);
	last = proc->nr_tasks == 1 && task->proc == proc;
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return last;
}

bool proc_detach_task(struct proc_struct *proc, struct task_struct *task)
{
	bool last = false;
	bool wake_exec;

	if (!proc || !task)
		return false;
	spin_lock(&proc_topology_lock);
	if (!list_empty(&task->proc_node)) {
		list_del_init(&task->proc_node);
		BUG_ON(proc->nr_tasks == 0);
		proc->nr_tasks--;
	}
	if (proc->leader == task) {
		proc->leader = NULL;
		if (!list_empty(&proc->tasks))
			proc->leader = list_first_entry(
				&proc->tasks, struct task_struct, proc_node);
	}
	last = proc->nr_tasks == 0;
	if (last && proc->lifecycle == PROC_LIVE)
		proc->lifecycle = PROC_EXITING;
	task->proc = NULL;
	wake_exec = proc->exec_owner != NULL;
	spin_unlock(&proc_topology_lock);
	proc_put(proc);
	if (wake_exec)
		wait_channel_wake_all(&proc->exec_channel);
	return last;
}

int proc_link_child(struct proc_struct *parent, struct proc_struct *child)
{
	if (!parent || !child || (child->parent && child->parent != parent))
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	if (parent->lifecycle >= PROC_EXITING) {
		spin_unlock(&proc_topology_lock);
		return -ESRCH;
	}
	if (!child->parent) {
		if (!proc_try_get(parent)) {
			spin_unlock(&proc_topology_lock);
			return -ESRCH;
		}
		child->parent = parent;
	}
	if (list_empty(&child->sibling))
		list_add_tail(&child->sibling, &parent->children);
	proc_update_pgrp_orphaned_locked(child->pgrp, NULL, 0, NULL);
	spin_unlock(&proc_topology_lock);
	return 0;
}

size_t proc_unlink_child(struct proc_struct *child,
			 struct proc_orphan_event *events, size_t capacity)
{
	struct proc_struct *parent;
	size_t count = 0;

	if (!child)
		return 0;
	spin_lock(&proc_topology_lock);
	parent = child->parent;
	child->parent = NULL;
	if (!list_empty(&child->sibling))
		list_del_init(&child->sibling);
	proc_update_pgrp_orphaned_locked(child->pgrp, events, capacity, &count);
	spin_unlock(&proc_topology_lock);
	if (parent)
		proc_put(parent);
	return count;
}

struct proc_struct *proc_parent_get(struct proc_struct *proc)
{
	struct proc_struct *parent = NULL;

	if (!proc)
		return NULL;
	spin_lock(&proc_topology_lock);
	if (proc->parent && proc_try_get(proc->parent))
		parent = proc->parent;
	spin_unlock(&proc_topology_lock);
	return parent;
}

struct task_struct *proc_leader_get(struct proc_struct *proc)
{
	struct task_struct *leader = NULL;

	if (!proc)
		return NULL;
	spin_lock(&proc_topology_lock);
	if (proc->leader && task_try_get(proc->leader))
		leader = proc->leader;
	spin_unlock(&proc_topology_lock);
	return leader;
}

bool proc_task_is_member(const struct proc_struct *proc,
			 const struct task_struct *task)
{
	bool member = false;

	if (!proc || !task)
		return false;
	spin_lock((spinlock_t *)&proc_topology_lock);
	member = task->proc == proc && !list_empty(&task->proc_node);
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return member;
}

bool proc_task_is_leader(const struct proc_struct *proc,
			 const struct task_struct *task)
{
	bool leader = false;

	if (!proc || !task)
		return false;
	spin_lock((spinlock_t *)&proc_topology_lock);
	leader = proc->leader == task && task->proc == proc &&
		 !list_empty(&task->proc_node);
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return leader;
}

bool proc_is_user_process(struct proc_struct *proc)
{
	bool user_process;

	if (!proc)
		return false;
	spin_lock(&proc->lock);
	user_process = proc->user_process;
	spin_unlock(&proc->lock);
	return user_process;
}

void proc_mark_user_process(struct proc_struct *proc)
{
	if (!proc)
		return;
	spin_lock(&proc->lock);
	proc->user_process = true;
	spin_unlock(&proc->lock);
}

void proc_inherit_user_process(struct proc_struct *child,
			       struct proc_struct *parent)
{
	bool user_process;

	if (!child || !parent)
		return;
	spin_lock(&parent->lock);
	user_process = parent->user_process;
	spin_unlock(&parent->lock);
	spin_lock(&child->lock);
	child->user_process = user_process;
	spin_unlock(&child->lock);
}

uint32_t proc_child_count(const struct proc_struct *proc)
{
	const struct proc_struct *child;
	uint32_t count = 0;

	if (!proc)
		return 0;
	spin_lock((spinlock_t *)&proc_topology_lock);
	list_for_each_entry (child, &proc->children, sibling)
		count++;
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return count;
}

size_t proc_task_snapshot(struct proc_struct *proc,
			  const struct task_struct *ignored,
			  struct task_struct **tasks, size_t capacity)
{
	struct task_struct *task;
	size_t count = 0;

	if (!proc || !tasks || capacity == 0)
		return 0;
	spin_lock(&proc_topology_lock);
	list_for_each_entry (task, &proc->tasks, proc_node) {
		if (task == ignored || count == capacity)
			continue;
		if (task_try_get(task))
			tasks[count++] = task;
	}
	spin_unlock(&proc_topology_lock);
	return count;
}

void proc_for_each_task(struct proc_struct *proc, proc_task_callback_t callback,
			void *arg)
{
	struct task_struct *task;

	if (!proc || !callback)
		return;
	spin_lock(&proc_topology_lock);
	list_for_each_entry (task, &proc->tasks, proc_node)
		callback(task, arg);
	spin_unlock(&proc_topology_lock);
}

size_t proc_pgrp_task_snapshot(struct pgrp_struct *pgrp,
			       struct session_struct *session,
			       struct task_struct **tasks, size_t capacity)
{
	struct proc_struct *member;
	size_t count = 0;

	if (!pgrp || !session || !tasks || capacity == 0)
		return 0;
	spin_lock(&proc_topology_lock);
	if (pgrp->session != session)
		goto out;
	/* Per-proc target must be a live task: an exiting leader must not
	 * make the whole process unreachable for process-directed signals. */
	list_for_each_entry (member, &pgrp->members, pgrp_node) {
		struct task_struct *task;

		if (member->lifecycle >= PROC_ZOMBIE)
			continue;
		list_for_each_entry (task, &member->tasks, proc_node) {
			if (count == capacity || task->lifecycle != TASK_LIVE)
				continue;
			if (task_try_get(task))
				tasks[count++] = task;
			break;
		}
	}
out:
	spin_unlock(&proc_topology_lock);
	return count;
}

static struct proc_struct *proc_reaper_locked(void)
{
	return init_task ? init_task->proc : NULL;
}

size_t proc_reparent_children(struct proc_struct *proc,
			      struct proc_orphan_event *events, size_t capacity)
{
	struct proc_struct *reaper;
	struct list_head *pos;
	struct list_head *next;
	size_t old_parent_refs = 0;
	size_t count = 0;

	if (!proc)
		return 0;
	spin_lock(&proc_topology_lock);
	reaper = proc_reaper_locked();
	if (reaper == proc)
		reaper = NULL;
	if (reaper && !proc_try_get(reaper))
		reaper = NULL;
	list_for_each_safe (pos, next, &proc->children) {
		struct proc_struct *child =
			list_entry(pos, struct proc_struct, sibling);

		list_del_init(&child->sibling);
		/* Topology invariant: a child linked into proc->children holds
		 * a parent reference to proc (proc_link_child takes it, and
		 * proc_unlink_child clears the pointer only after unlinking).
		 * Count the refs instead of stacking them; they are all proc
		 * and must be released outside the lock. */
		BUG_ON(child->parent != proc);
		old_parent_refs++;
		child->parent = reaper;
		if (reaper) {
			proc_get(reaper);
			list_add_tail(&child->sibling, &reaper->children);
		}
		proc_update_pgrp_orphaned_locked(child->pgrp, events, capacity,
						 &count);
	}
	spin_unlock(&proc_topology_lock);
	for (size_t index = 0; index < old_parent_refs; index++)
		proc_put(proc);
	if (reaper)
		proc_put(reaper);
	return count;
}

bool proc_begin_group_exit(struct proc_struct *proc, int status)
{
	bool first = false;

	if (!proc)
		return false;
	spin_lock(&proc_topology_lock);
	if (proc->lifecycle == PROC_LIVE) {
		proc->lifecycle = PROC_EXITING;
		proc->group_exit_status = status;
		proc->group_exit_status_valid = true;
		proc->group_exit_requested = true;
		first = true;
	}
	spin_unlock(&proc_topology_lock);
	return first;
}

bool proc_group_exit_pending(const struct proc_struct *proc, int *status)
{
	bool pending;

	if (!proc || !status)
		return false;
	spin_lock((spinlock_t *)&proc_topology_lock);
	pending = proc->lifecycle == PROC_EXITING &&
		  proc->group_exit_requested && proc->group_exit_status_valid;
	if (pending)
		*status = proc->group_exit_status;
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return pending;
}

static void proc_parent_event_init(struct proc_parent_event *event)
{
	if (event)
		memset(event, 0, sizeof(*event));
}

static void proc_parent_event_cputime_locked(
		struct proc_struct *proc, struct task_cputime *cputime)
{
	const struct task_struct *task;

	*cputime = (struct task_cputime){0};
	spin_lock(&proc->lock);
	*cputime = proc->cputime;
	spin_unlock(&proc->lock);
	list_for_each_entry (task, &proc->tasks, proc_node) {
		if (task->lifecycle == TASK_LIVE)
			cputime_add(cputime, &task->cputime);
	}
}

static void proc_parent_event_fill_locked(struct proc_struct *proc, int code,
					  int status, uid_t uid,
					  struct proc_parent_event *event)
{
	if (!event || !proc->parent || !proc_try_get(proc->parent))
		return;
	if (!proc_try_get(proc)) {
		proc_put(proc->parent);
		return;
	}
	event->parent = proc->parent;
	event->child = proc;
	event->pid = proc->pid ? proc->pid->nr : 0;
	event->uid = uid;
	event->signal = proc->wait_state.exit_signal;
	event->code = code;
	event->status = status;
	proc_parent_event_cputime_locked(proc, &event->cputime);
}

static uint64_t proc_wait_event_generation(const struct proc_wait_state *wait,
					   uint32_t event)
{
	if (!wait)
		return 0;
	switch (event) {
	case PROC_WAIT_EXIT:
		return wait->exit_generation;
	case PROC_WAIT_STOP:
		return wait->stop_generation;
	case PROC_WAIT_CONTINUE:
		return wait->continue_generation;
	default:
		return 0;
	}
}

static uint64_t proc_wait_claimed_generation(const struct proc_wait_state *wait,
					     uint32_t event)
{
	if (!wait)
		return 0;
	switch (event) {
	case PROC_WAIT_EXIT:
		return wait->claimed_exit_generation;
	case PROC_WAIT_STOP:
		return wait->claimed_stop_generation;
	case PROC_WAIT_CONTINUE:
		return wait->claimed_continue_generation;
	default:
		return 0;
	}
}

static void proc_wait_set_claimed_generation(struct proc_wait_state *wait,
					     uint32_t event,
					     uint64_t generation)
{
	if (!wait)
		return;
	switch (event) {
	case PROC_WAIT_EXIT:
		wait->claimed_exit_generation = generation;
		break;
	case PROC_WAIT_STOP:
		wait->claimed_stop_generation = generation;
		break;
	case PROC_WAIT_CONTINUE:
		wait->claimed_continue_generation = generation;
		break;
	default:
		break;
	}
}

static bool proc_wait_event_claimable(const struct proc_wait_state *wait,
				      uint32_t event)
{
	uint64_t generation;

	if (!wait || !(wait->pending & event))
		return false;
	generation = proc_wait_event_generation(wait, event);
	return generation != 0 &&
	       generation != proc_wait_claimed_generation(wait, event);
}

static bool proc_all_tasks_stopped_locked(const struct proc_struct *proc)
{
	const struct task_struct *task;

	list_for_each_entry (task, &proc->tasks, proc_node) {
		if (task->run_state != TASK_STOPPED &&
		    task->lifecycle != TASK_EXITING)
			return false;
	}
	return true;
}

static bool proc_has_stopped_task_locked(const struct proc_struct *proc)
{
	const struct task_struct *task;

	list_for_each_entry (task, &proc->tasks, proc_node) {
		if (task->run_state == TASK_STOPPED)
			return true;
	}
	return false;
}

void proc_publish_stop(struct proc_struct *proc, int sig,
		       struct proc_parent_event *event)
{
	bool publish = false;

	proc_parent_event_init(event);
	if (!proc)
		return;
	/* Task stop transitions are aggregated: the wait-visible event only fires
	 * once the whole Proc is stopped (an exiting task counts as stopped). */
	spin_lock(&proc_topology_lock);
	if (!proc->job_stopped && proc_all_tasks_stopped_locked(proc)) {
		irq_flags_t flags;

		proc->job_stopped = true;
		publish = true;
		spin_lock_irqsave(&proc->wait_state.lock, &flags);
		proc->wait_state.stop_status = (sig << 8) | 0x7f;
		proc->wait_state.pending |= PROC_WAIT_STOP;
		proc->wait_state.stop_generation =
			++proc->wait_state.generation;
		spin_unlock_irqrestore(&proc->wait_state.lock, flags);
	}
	if (publish)
		proc_parent_event_fill_locked(
			proc, CLD_STOPPED, sig,
			proc->leader ? task_uid(proc->leader) : 0, event);
	spin_unlock(&proc_topology_lock);
}

void proc_publish_continue(struct proc_struct *proc,
			   struct proc_parent_event *event)
{
	bool publish = false;

	proc_parent_event_init(event);
	if (!proc)
		return;
	spin_lock(&proc_topology_lock);
	/* Clear only when no task remains stopped, so CLD_CONTINUED matches
	 * the whole-proc resume state. */
	if (proc->job_stopped && !proc_has_stopped_task_locked(proc)) {
		irq_flags_t flags;

		proc->job_stopped = false;
		publish = true;
		spin_lock_irqsave(&proc->wait_state.lock, &flags);
		proc->wait_state.continue_status = 0xffff;
		proc->wait_state.pending |= PROC_WAIT_CONTINUE;
		proc->wait_state.continue_generation =
			++proc->wait_state.generation;
		spin_unlock_irqrestore(&proc->wait_state.lock, flags);
	}
	if (publish)
		proc_parent_event_fill_locked(
			proc, CLD_CONTINUED, SIGCONT,
			proc->leader ? task_uid(proc->leader) : 0, event);
	spin_unlock(&proc_topology_lock);
}

void proc_prepare_exit(struct proc_struct *proc, int status, uid_t uid,
		       bool auto_reap, bool notify_sigchld)

{
	if (!proc)
		return;
	spin_lock(&proc_topology_lock);
	BUG_ON(proc->nr_tasks != 0);
	BUG_ON(proc->lifecycle != PROC_EXITING);
	proc->exit_status = status;
	proc->exit_uid = uid;
	/* Snapshot the group identity while the proc is still attached to its
	 * pgrp: a zombie must answer getpgid/getsid until wait4 reaps it, even
	 * though its leader is gone and the pgrp unlink is imminent. */
	if (proc->pgrp && proc->pgrp->pgid && proc->pgrp->session &&
	    proc->pgrp->session->sid) {
		proc->exit_pgid = proc->pgrp->pgid->nr;
		proc->exit_sid = proc->pgrp->session->sid->nr;
	} else {
		proc->exit_pgid = 0;
		proc->exit_sid = 0;
	}
	proc->exit_pending = true;
	proc->exit_auto_reap = auto_reap;
	proc->exit_sigchld_notify = notify_sigchld;
	spin_unlock(&proc_topology_lock);
}

size_t proc_publish_exit(struct proc_struct *proc,
			 struct proc_orphan_event *events, size_t capacity,
			 struct proc_parent_event *parent_event)
{
	size_t orphan_count;
	bool has_parent;
	int status;
	uid_t uid;
	int code;
	int info_status;
	bool auto_reap;
	irq_flags_t flags;

	if (!proc)
		return 0;
	proc_parent_event_init(parent_event);
	spin_lock(&proc_topology_lock);
	BUG_ON(proc->nr_tasks != 0);
	BUG_ON(proc->lifecycle != PROC_EXITING || !proc->exit_pending);
	status = proc->exit_status;
	uid = proc->exit_uid;
	auto_reap = proc->exit_auto_reap;
	proc->exit_pending = false;
	has_parent = proc->parent != NULL;
	if (status & 0x7f) {
		code = (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
		info_status = status & 0x7f;
	} else {
		code = CLD_EXITED;
		info_status = (status >> 8) & 0xff;
	}
	proc->wait_state.exit_uid = uid;
	proc_parent_event_fill_locked(proc, code, info_status, uid,
				      parent_event);
	spin_unlock(&proc_topology_lock);
	orphan_count = proc_reparent_children(proc, events, capacity);
	spin_lock(&proc_topology_lock);
	spin_lock_irqsave(&proc->wait_state.lock, &flags);
	proc->wait_state.exit_status = status;
	if (!auto_reap)
		proc->wait_state.pending |= PROC_WAIT_EXIT;
	proc->wait_state.exit_generation = ++proc->wait_state.generation;
	proc->lifecycle = has_parent && !auto_reap ? PROC_ZOMBIE : PROC_DEAD;
	spin_unlock_irqrestore(&proc->wait_state.lock, flags);
	/* An exiting proc leaves its pgrp immediately: orphan evaluation must
	 * not wait for reaping, because a zombie keeps its parent reference
	 * until wait4 and would otherwise delay the SIGHUP/SIGCONT event. */
	if (!list_empty(&proc->pgrp_node)) {
		list_del_init(&proc->pgrp_node);
		proc_update_pgrp_orphaned_locked(proc->pgrp, events, capacity,
						 &orphan_count);
	}
	spin_unlock(&proc_topology_lock);
	if (!has_parent || auto_reap) {
		if (orphan_count < capacity)
			orphan_count +=
				proc_unlink_child(proc, &events[orphan_count],
						  capacity - orphan_count);
		else
			(void)proc_unlink_child(proc, NULL, 0);
		proc_unpublish(proc);
	}
	return orphan_count;
}

bool proc_can_reap(const struct proc_struct *proc)
{
	return proc && proc->lifecycle == PROC_ZOMBIE && proc->nr_tasks == 0;
}

size_t proc_mark_reaped(struct proc_struct *proc,
			struct proc_orphan_event *events, size_t capacity)
{
	size_t count;

	if (!proc)
		return 0;
	spin_lock(&proc_topology_lock);
	BUG_ON(!proc_can_reap(proc));
	proc->lifecycle = PROC_DEAD;
	spin_unlock(&proc_topology_lock);
	proc_unpublish(proc);
	count = proc_unlink_child(proc, events, capacity);
	return count;
}

int proc_exec_begin(struct proc_struct *proc, struct task_struct *owner)
{
	int ret = 0;

	if (!proc || !owner)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	if (proc->lifecycle != PROC_LIVE || owner->proc != proc ||
	    list_empty(&owner->proc_node))
		ret = -ESRCH;
	else if (proc->exec_owner)
		ret = -EBUSY;
	else
		proc->exec_owner = owner;
	spin_unlock(&proc_topology_lock);
	return ret;
}

int proc_exec_request_siblings(struct proc_struct *proc,
			       struct task_struct *owner)
{
	struct task_struct *task;
	struct task_struct **targets;
	size_t count = 0;

	targets = kmalloc_array(PID_COUNT, sizeof(*targets), ALLOC_NOWAIT);
	if (!targets)
		return -ENOMEM;

	spin_lock(&proc_topology_lock);
	if (proc->exec_owner != owner || proc->lifecycle != PROC_LIVE) {
		spin_unlock(&proc_topology_lock);
		kfree(targets);
		return -EINTR;
	}
	list_for_each_entry (task, &proc->tasks, proc_node) {
		if (task == owner || task->lifecycle != TASK_LIVE ||
		    count == PID_COUNT)
			continue;
		if (task_try_get(task))
			targets[count++] = task;
	}
	spin_unlock(&proc_topology_lock);

	for (size_t index = 0; index < count; index++) {
		if (!task_request_exec_exit(targets[index])) {
			task_put(targets[index]);
			continue;
		}
		task_put(targets[index]);
	}
	kfree(targets);
	return 0;
}

int proc_exec_wait(struct proc_struct *proc, struct task_wait *wait)
{
	int ret;

	if (!proc || !wait)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	if (proc->exec_owner != current_task())
		ret = -EPERM;
	else if (proc->lifecycle != PROC_LIVE)
		ret = -EINTR;
	else if (proc->nr_tasks == 1)
		ret = 1;
	else
		ret = wait_prepare(wait, &proc->exec_channel, true);
	spin_unlock(&proc_topology_lock);
	return ret;
}

int proc_exec_adopt_pid(struct proc_struct *proc, struct task_struct *owner)
{
	struct pid_identity *old_tid;
	struct pid_identity *proc_pid;
	bool drop_registry = false;
	int ret;

	if (!proc || !owner)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	if (proc->exec_owner != owner || proc->nr_tasks != 1 ||
	    owner->proc != proc || proc->leader != owner) {
		spin_unlock(&proc_topology_lock);
		return -EINVAL;
	}
	old_tid = owner->tid;
	proc_pid = proc->pid;
	if (old_tid == proc_pid) {
		spin_unlock(&proc_topology_lock);
		return 0;
	}
	ret = pid_transfer_task_role(old_tid, proc_pid, owner, &drop_registry);
	if (ret == 0) {
		owner->tid = proc_pid;
	}
	spin_unlock(&proc_topology_lock);
	if (ret == 0) {
		if (drop_registry)
			pid_put(old_tid); /* release the deferred registry
					     reference */
		pid_put(old_tid); /* release the task's old identity reference
				   */
	}
	return ret;
}

void proc_exec_end(struct proc_struct *proc, struct task_struct *owner)
{
	bool wake = false;

	if (!proc || !owner)
		return;
	spin_lock(&proc_topology_lock);
	if (proc->exec_owner == owner) {
		proc->exec_owner = NULL;
		wake = true;
	}
	spin_unlock(&proc_topology_lock);
	if (wake)
		wait_channel_wake_all(&proc->exec_channel);
}

static bool proc_wait_matches(const struct proc_struct *parent,
			      const struct proc_struct *child,
			      const struct proc_wait_selector *selector)
{
	pid_t pid;

	if (!selector)
		return false;
	pid = selector->pid;
	if (!parent || !child || child->lifecycle == PROC_DEAD)
		return false;
	if (selector->creator_only &&
	    child->wait_state.creator_tid != selector->creator_tid)
		return false;
	if (selector->child_class == PROC_WAIT_SIGCHLD &&
	    child->wait_state.exit_signal != SIGCHLD)
		return false;
	if (selector->child_class == PROC_WAIT_CLONE &&
	    child->wait_state.exit_signal == SIGCHLD)
		return false;
	if (pid > 0)
		return child->pid && child->pid->nr == pid;
	if (pid == 0)
		return child->pgrp && parent->pgrp &&
		       child->pgrp->pgid->nr == parent->pgrp->pgid->nr;
	if (pid < -1)
		return child->pgrp && child->pgrp->pgid->nr == -pid;
	return true;
}

static uint32_t proc_wait_first_event(const struct proc_wait_state *wait,
				      uint32_t event_mask)
{
	if ((event_mask & PROC_WAIT_EXIT) &&
	    proc_wait_event_claimable(wait, PROC_WAIT_EXIT))
		return PROC_WAIT_EXIT;
	if ((event_mask & PROC_WAIT_STOP) &&
	    proc_wait_event_claimable(wait, PROC_WAIT_STOP))
		return PROC_WAIT_STOP;
	if ((event_mask & PROC_WAIT_CONTINUE) &&
	    proc_wait_event_claimable(wait, PROC_WAIT_CONTINUE))
		return PROC_WAIT_CONTINUE;
	return 0;
}

static bool proc_wait_event_available_locked(struct proc_struct *parent,
				 const struct proc_wait_selector *selector,
				 uint32_t event_mask)
{
	struct proc_struct *child;
	irq_flags_t flags;

	list_for_each_entry (child, &parent->children, sibling) {
		if (!proc_wait_matches(parent, child, selector))
			continue;
		spin_lock_irqsave(&child->wait_state.lock, &flags);
		if (proc_wait_first_event(&child->wait_state, event_mask) !=
		    0) {
			spin_unlock_irqrestore(&child->wait_state.lock, flags);
			return true;
		}
		spin_unlock_irqrestore(&child->wait_state.lock, flags);
	}
	return false;
}

enum proc_wait_result proc_wait_claim(struct proc_struct *parent,
				      const struct proc_wait_selector *selector,
				      uint32_t event_mask,
				      struct proc_wait_claim *claim)
{
	struct proc_struct *child;
	bool has_child = false;
	irq_flags_t flags;

	if (!parent || !selector || !claim)
		return PROC_WAIT_NO_CHILD;
	/* Clear only the claim state owned by proc wait.  The orphan-event
	 * buffer is owned by the caller and must survive across calls. */
	claim->parent = NULL;
	claim->child = NULL;
	claim->generation = 0;
	claim->event = 0;
	claim->status = 0;
	claim->orphan_count = 0;
	spin_lock(&proc_topology_lock);
	list_for_each_entry (child, &parent->children, sibling) {
		uint32_t event;

		if (!proc_wait_matches(parent, child, selector))
			continue;
		has_child = true;
		spin_lock_irqsave(&child->wait_state.lock, &flags);
		event = proc_wait_first_event(&child->wait_state, event_mask);
		if (!event) {
			spin_unlock_irqrestore(&child->wait_state.lock, flags);
			continue;
		}
		if (!proc_try_get(child)) {
			spin_unlock_irqrestore(&child->wait_state.lock, flags);
			continue;
		}
		spin_unlock_irqrestore(&child->wait_state.lock, flags);
		/* The parent reference is an atomic try_get and may run while
		 * the topology lock is held; the child reference release must
		 * not (a final put would destroy the child from inside a lock).
		 */
		if (!proc_try_get(parent)) {
			spin_unlock(&proc_topology_lock);
			proc_put(child);
			return PROC_WAIT_NO_CHILD;
		}
		spin_lock_irqsave(&child->wait_state.lock, &flags);
		/* Re-check under the lock: another claimer may have consumed
		 * the event while the wait lock was dropped.  The puts below
		 * return try_get references only, never a final put. */
		event = proc_wait_first_event(&child->wait_state, event_mask);
		if (!event) {
			spin_unlock_irqrestore(&child->wait_state.lock, flags);
			proc_put(child);
			proc_put(parent);
			continue;
		}
		claim->parent = parent;
		claim->child = child;
		claim->generation =
			proc_wait_event_generation(&child->wait_state, event);
		proc_wait_set_claimed_generation(&child->wait_state, event,
						 claim->generation);
		claim->event = event;
		claim->status = event == PROC_WAIT_EXIT
					? child->wait_state.exit_status
				: event == PROC_WAIT_STOP
					? child->wait_state.stop_status
					: child->wait_state.continue_status;
		spin_unlock_irqrestore(&child->wait_state.lock, flags);
		spin_unlock(&proc_topology_lock);
		return PROC_WAIT_EVENT;
	}
	spin_unlock(&proc_topology_lock);
	return has_child ? PROC_WAIT_NO_EVENT : PROC_WAIT_NO_CHILD;
}

int proc_wait_watch(struct proc_struct *parent,
		    const struct proc_wait_selector *selector,
		    uint32_t event_mask, struct task_wait *wait)
{
	bool wake = false;
	int ret;

	spin_lock(&proc_topology_lock);
	if (proc_wait_event_available_locked(parent, selector, event_mask)) {
		ret = 1;
	} else {
		ret = wait_prepare(wait, &parent->wait_state.channel, true);
		if (ret == 0 && proc_wait_event_available_locked(
					parent, selector, event_mask))
			wake = true;
	}
	spin_unlock(&proc_topology_lock);
	if (wake)
		wait_channel_wake_one(&parent->wait_state.channel);
	return ret;
}

bool proc_wait_commit(struct proc_wait_claim *claim)
{
	struct proc_struct *child;
	bool committed = false;
	irq_flags_t flags;

	if (!claim || !claim->child)
		return false;
	child = claim->child;
	spin_lock_irqsave(&child->wait_state.lock, &flags);
	if (proc_wait_claimed_generation(&child->wait_state, claim->event) ==
	    claim->generation) {
		child->wait_state.pending &= ~claim->event;
		proc_wait_set_claimed_generation(&child->wait_state,
						 claim->event, 0);
		committed = true;
	}
	spin_unlock_irqrestore(&child->wait_state.lock, flags);
	if (committed && claim->event == PROC_WAIT_EXIT)
		claim->orphan_count =
			proc_mark_reaped(child, claim->orphan_events,
					 claim->orphan_capacity);
	proc_put(child);
	proc_put(claim->parent);
	claim->child = NULL;
	claim->parent = NULL;
	return committed;
}

void proc_wait_abort(struct proc_wait_claim *claim)
{
	irq_flags_t flags;

	if (!claim || !claim->child)
		return;
	spin_lock_irqsave(&claim->child->wait_state.lock, &flags);
	if (proc_wait_claimed_generation(&claim->child->wait_state,
					 claim->event) == claim->generation)
		proc_wait_set_claimed_generation(&claim->child->wait_state,
						 claim->event, 0);
	spin_unlock_irqrestore(&claim->child->wait_state.lock, flags);
	wait_channel_wake_one(&claim->parent->wait_state.channel);
	proc_put(claim->child);
	proc_put(claim->parent);
	memset(claim, 0, sizeof(*claim));
}

int proc_snapshot_topology(const struct proc_struct *proc, pid_t *pgid,
			   pid_t *sid)
{
	if (!proc || !pgid || !sid)
		return -EINVAL;
	spin_lock((spinlock_t *)&proc_topology_lock);
	if (!proc->pgrp || !proc->pgrp->session || !proc->pgrp->pgid ||
	    !proc->pgrp->session->sid) {
		spin_unlock((spinlock_t *)&proc_topology_lock);
		return -ESRCH;
	}
	*pgid = proc->pgrp->pgid->nr;
	*sid = proc->pgrp->session->sid->nr;
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return 0;
}

bool proc_exit_identity(const struct proc_struct *proc, pid_t *pgid, pid_t *sid)
{
	bool have = false;

	if (!proc || !pgid || !sid)
		return false;
	spin_lock(&proc_topology_lock);
	if (proc->exit_pgid > 0) {
		*pgid = proc->exit_pgid;
		*sid = proc->exit_sid;
		have = true;
	}
	spin_unlock(&proc_topology_lock);
	return have;
}

struct pgrp_struct *proc_lookup_pgrp(pid_t pgid)
{
	return pid_lookup_pgrp(pgid);
}

struct session_struct *proc_lookup_session(pid_t sid)
{
	return pid_lookup_session(sid);
}

/**
 * @brief Report whether a session has any pgrp with remaining members.
 *
 * Callers that hold the TTY lock may call this: the check runs under the
 * topology lock (rank 60), which orders after session (10) and TTY (20).
 * No references are taken or released.  @p exclude is ignored for the
 * emptiness verdict (typically the exiting last member of the session).
 */
bool proc_session_is_empty(const struct session_struct *session,
			   const struct proc_struct *exclude)
{
	const struct pgrp_struct *pgrp;
	bool empty = true;

	if (!session)
		return true;
	spin_lock((spinlock_t *)&proc_topology_lock);
	list_for_each_entry (pgrp, &session->pgrps, session_node) {
		const struct proc_struct *proc;

		list_for_each_entry (proc, &pgrp->members, pgrp_node) {
			if (proc == exclude)
				continue;
			empty = false;
			break;
		}
		if (!empty)
			break;
	}
	spin_unlock((spinlock_t *)&proc_topology_lock);
	return empty;
}

bool proc_pgrp_has_member(pid_t pgid, pid_t sid,
			  const struct task_struct *ignored)
{
	struct pgrp_struct *pgrp;
	struct proc_struct *member;
	bool found = false;

	pgrp = proc_lookup_pgrp(pgid);
	if (!pgrp || !pgrp->session || !pgrp->session->sid ||
	    pgrp->session->sid->nr != sid)
		goto out;
	spin_lock((spinlock_t *)&proc_topology_lock);
	/* Liveness is task-granular: a proc whose leader exits but still has
	 * live siblings keeps counting, while an exiting single-task proc
	 * (its only task is TASK_EXITING) stops counting. */
	list_for_each_entry (member, &pgrp->members, pgrp_node) {
		struct task_struct *task;

		if (member->lifecycle >= PROC_ZOMBIE)
			continue;
		list_for_each_entry (task, &member->tasks, proc_node) {
			if (task != ignored && task->lifecycle == TASK_LIVE) {
				found = true;
				break;
			}
		}
		if (found)
			break;
	}
	spin_unlock((spinlock_t *)&proc_topology_lock);
out:
	proc_pgrp_put(pgrp);
	return found;
}

int proc_join_pgrp(struct proc_struct *proc, pid_t pgid,
		   struct proc_orphan_event *event)
{
	struct pgrp_struct *new_pgrp;
	struct pgrp_struct *old_pgrp;
	struct session_struct *session;
	struct pid_identity *identity = NULL;
	bool moved = false;
	int ret = 0;

	if (event)
		memset(event, 0, sizeof(*event));
	if (!proc || !proc->pgrp || pgid <= 0)
		return -EINVAL;
	spin_lock(&proc_topology_lock);
	old_pgrp = proc->pgrp;
	session = old_pgrp->session;
	proc_pgrp_get(old_pgrp);
	proc_session_get(session);
	spin_unlock(&proc_topology_lock);

	new_pgrp = proc_lookup_pgrp(pgid);
	if (!new_pgrp) {
		identity = pid_lookup_identity(pgid);
		if (!identity) {
			ret = -ESRCH;
			goto out_refs;
		}
		new_pgrp = pgrp_alloc(identity, session);
		if (!new_pgrp) {
			ret = -ENOMEM;
			goto out_refs;
		}
	}
	spin_lock(&proc_topology_lock);
	/* new_pgrp->session and proc->pgrp are topology-owned: the session
	 * check must run under the topology lock, not between lock
	 * acquisitions. */
	if (new_pgrp->session != session) {
		ret = -EPERM;
	} else if (proc->pgrp != old_pgrp || proc->lifecycle >= PROC_EXITING) {
		ret = -ESRCH;
	} else if (old_pgrp != new_pgrp) {
		if (!list_empty(&proc->pgrp_node))
			list_del_init(&proc->pgrp_node);
		proc->pgrp = new_pgrp;
		list_add_tail(&proc->pgrp_node, &new_pgrp->members);
		{
			size_t count = 0;

			proc_update_pgrp_orphaned_locked(old_pgrp, event,
							 event ? 1 : 0, &count);
		}
		proc_update_pgrp_orphaned_locked(new_pgrp, NULL, 0, NULL);
		moved = true;
	}
	spin_unlock(&proc_topology_lock);

	if (ret < 0 || old_pgrp == new_pgrp)
		proc_pgrp_put(new_pgrp);
out_refs:
	if (identity)
		pid_put(identity);
	proc_pgrp_put(old_pgrp);
	if (moved)
		proc_pgrp_put(old_pgrp);
	proc_session_put(session);
	return ret;
}

int proc_create_session(struct proc_struct *proc, pid_t *sid,
			struct proc_orphan_event *events, size_t capacity,
			size_t *event_count)
{
	struct session_struct *session;
	struct pgrp_struct *pgrp;
	struct pgrp_struct *old_pgrp;
	struct session_struct *old_session;
	struct pid_identity *identity;
	size_t count = 0;
	bool moved = false;
	bool pgid_match = false;
	int ret = 0;

	if (event_count)
		*event_count = 0;
	if (events && capacity)
		memset(events, 0, capacity * sizeof(*events));
	if (!proc || !proc->pid || !sid)
		return -EINVAL;
	identity = proc->pid;
	pid_get(identity);
	spin_lock(&proc_topology_lock);
	old_pgrp = proc->pgrp;
	old_session = old_pgrp ? old_pgrp->session : NULL;
	if (old_pgrp)
		proc_pgrp_get(old_pgrp);
	if (old_session)
		proc_session_get(old_session);
	/* old_pgrp->pgid is topology-owned: capture the leader check under
	 * the lock instead of reading it after unlock. */
	pgid_match = old_pgrp && old_pgrp->pgid == identity;
	spin_unlock(&proc_topology_lock);
	if (pgid_match)
		ret = -EPERM;
	if (ret < 0)
		goto out_old;
	session = session_alloc(identity);
	if (!session) {
		ret = -ENOMEM;
		goto out_old;
	}
	pgrp = pgrp_alloc(identity, session);
	if (!pgrp) {
		proc_session_put(session);
		ret = -ENOMEM;
		goto out_old;
	}
	spin_lock(&proc_topology_lock);
	if (proc->pgrp != old_pgrp || proc->lifecycle >= PROC_EXITING) {
		ret = -ESRCH;
	} else {
		struct pgrp_struct *iter;

		if (!list_empty(&proc->pgrp_node))
			list_del_init(&proc->pgrp_node);
		proc->pgrp = pgrp;
		list_add_tail(&proc->pgrp_node, &pgrp->members);
		/* The caller leaves the whole session, so every pgrp that
		 * relied on the caller as its only same-session outside
		 * parent may now be orphaned: re-evaluate all pgrps of the
		 * old session, not just the caller's former group. */
		if (old_session)
			list_for_each_entry (iter, &old_session->pgrps,
					     session_node)
				proc_update_pgrp_orphaned_locked(
					iter, events, capacity, &count);
		moved = true;
	}
	spin_unlock(&proc_topology_lock);
	if (ret < 0)
		proc_pgrp_put(pgrp);
	else
		*sid = identity->nr;
	proc_session_put(session);

out_old:
	proc_pgrp_put(old_pgrp);
	if (moved)
		proc_pgrp_put(old_pgrp);
	proc_session_put(old_session);
	pid_put(identity);
	if (event_count)
		*event_count = count;
	return ret < 0 ? ret : *sid;
}

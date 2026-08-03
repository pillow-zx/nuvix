/*
 * kernel/tty.c - single-console terminal state and process attachments
 */

#include <kernel/errno.h>
#include <kernel/list.h>
#include <kernel/slab.h>
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/tty.h>

#include "tty_internal.h"

struct tty_endpoint {
	mutex_t lock;
	pid_t session;
	pid_t foreground_pgid;
	struct list_head attachments;
};

struct tty_process_attachment {
	pid_t leader_pid;
	pid_t session;
	struct list_head node;
};

static struct tty_endpoint console_tty = {
	.lock = MUTEX_INIT(console_tty.lock),
	.attachments = LIST_HEAD_INIT(console_tty.attachments),
};

static struct task_struct *tty_task_leader(struct task_struct *task)
{
	return task_group_leader_safe(task);
}

static struct tty_process_attachment *
tty_find_attachment_locked(struct tty_endpoint *tty, pid_t leader_pid)
{
	struct tty_process_attachment *attachment;

	list_for_each_entry (attachment, &tty->attachments, node) {
		if (attachment->leader_pid == leader_pid)
			return attachment;
	}
	return NULL;
}

static void tty_ctty_state_clear(struct tty_ctty_state *state)
{
	if (!state)
		return;
	state->sid = 0;
	state->foreground_pgid = 0;
}

static void tty_ctty_state_set_locked(struct tty_endpoint *tty,
				      struct tty_ctty_state *state)
{
	if (!state)
		return;
	state->sid = tty->session;
	state->foreground_pgid = tty->foreground_pgid;
}

static bool tty_has_owner_locked(const struct tty_endpoint *tty)
{
	return tty->session > 0;
}

static bool tty_ctty_owned_by_locked(struct tty_endpoint *tty,
				     struct task_struct *task, pid_t sid)
{
	struct task_struct *leader = tty_task_leader(task);
	struct tty_process_attachment *attachment;

	if (!leader || sid <= 0 || !tty_has_owner_locked(tty) ||
	    tty->session != sid)
		return false;
	attachment = tty_find_attachment_locked(tty, task_pid(leader));
	return attachment && attachment->session == sid;
}

static void
tty_move_attachment_locked(struct tty_process_attachment *attachment,
			   struct list_head *released)
{
	list_del_init(&attachment->node);
	list_add_tail(&attachment->node, released);
}

static void tty_free_attachments(struct list_head *released)
{
	struct list_head *pos;
	struct list_head *next;

	list_for_each_safe (pos, next, released) {
		struct tty_process_attachment *attachment =
			list_entry(pos, struct tty_process_attachment, node);

		list_del_init(&attachment->node);
		kfree(attachment);
	}
}

static bool tty_detach_session_locked(struct tty_endpoint *tty,
				      struct tty_ctty_state *detached,
				      struct list_head *released)
{
	struct list_head *pos;
	struct list_head *next;

	if (!tty_has_owner_locked(tty))
		return false;
	tty_ctty_state_set_locked(tty, detached);
	list_for_each_safe (pos, next, &tty->attachments) {
		struct tty_process_attachment *attachment =
			list_entry(pos, struct tty_process_attachment, node);

		tty_move_attachment_locked(attachment, released);
	}
	tty->session = 0;
	tty->foreground_pgid = 0;
	return true;
}

void tty_console_endpoint_init(void)
{
	mutex_init(&console_tty.lock);
	console_tty.session = 0;
	console_tty.foreground_pgid = 0;
	INIT_LIST_HEAD(&console_tty.attachments);
}

struct tty_endpoint *tty_console_endpoint(void)
{
	return &console_tty;
}

int tty_ctty_clone_attachment(struct task_struct *parent,
			      struct task_struct *child)
{
	struct task_struct *parent_leader = tty_task_leader(parent);
	struct task_struct *child_leader = tty_task_leader(child);
	struct tty_process_attachment *parent_attachment;
	struct tty_process_attachment *child_attachment;
	pid_t session;

	if (!parent_leader || !child_leader)
		return -ESRCH;

	mutex_lock(&console_tty.lock);
	parent_attachment = tty_find_attachment_locked(&console_tty,
						       task_pid(parent_leader));
	if (!parent_attachment) {
		mutex_unlock(&console_tty.lock);
		return 0;
	}
	session = parent_attachment->session;
	mutex_unlock(&console_tty.lock);

	child_attachment = kzalloc(sizeof(*child_attachment), ALLOC_NOWAIT);
	if (!child_attachment)
		return -ENOMEM;
	INIT_LIST_HEAD(&child_attachment->node);

	mutex_lock(&console_tty.lock);
	parent_attachment = tty_find_attachment_locked(&console_tty,
						       task_pid(parent_leader));
	if (parent_attachment && parent_attachment->session == session) {
		BUG_ON(tty_find_attachment_locked(&console_tty,
						  task_pid(child_leader)));
		child_attachment->leader_pid = task_pid(child_leader);
		child_attachment->session = session;
		list_add_tail(&child_attachment->node,
			      &console_tty.attachments);
		child_attachment = NULL;
	}
	mutex_unlock(&console_tty.lock);

	kfree(child_attachment);
	return 0;
}

void tty_ctty_remove_task(struct task_struct *task, pid_t sid,
			  enum tty_ctty_remove_scope scope,
			  struct tty_ctty_state *detached)
{
	LIST_HEAD(released);
	struct task_struct *leader = tty_task_leader(task);
	struct tty_process_attachment *attachment;

	tty_ctty_state_clear(detached);
	if (!leader)
		return;

	mutex_lock(&console_tty.lock);
	attachment = tty_find_attachment_locked(&console_tty, task_pid(leader));
	if (scope == TTY_CTTY_REVOKE_SESSION && console_tty.session == sid) {
		(void)tty_detach_session_locked(&console_tty, detached,
						&released);
	} else if (attachment && attachment->session == sid) {
		tty_move_attachment_locked(attachment, &released);
	}
	mutex_unlock(&console_tty.lock);
	tty_free_attachments(&released);
}

bool tty_ctty_has_owner(struct tty_endpoint *tty, struct tty_ctty_state *state)
{
	bool has_owner;

	if (!tty) {
		tty_ctty_state_clear(state);
		return false;
	}

	mutex_lock(&tty->lock);
	has_owner = tty_has_owner_locked(tty);
	if (has_owner)
		tty_ctty_state_set_locked(tty, state);
	else
		tty_ctty_state_clear(state);
	mutex_unlock(&tty->lock);
	return has_owner;
}

int tty_ctty_claim(struct tty_endpoint *tty, struct task_struct *task,
		   pid_t sid, pid_t pgid, bool steal,
		   struct tty_ctty_state *displaced)
{
	LIST_HEAD(released);
	struct task_struct *leader = tty_task_leader(task);
	struct tty_process_attachment *attachment;
	struct tty_process_attachment *fresh;
	int ret = 0;

	if (!tty || !leader || sid <= 0 || pgid <= 0)
		return -EINVAL;

	fresh = kzalloc(sizeof(*fresh), ALLOC_NOWAIT);
	if (!fresh)
		return -ENOMEM;
	INIT_LIST_HEAD(&fresh->node);
	tty_ctty_state_clear(displaced);

	mutex_lock(&tty->lock);
	if (tty_has_owner_locked(tty) && tty->session != sid) {
		if (!steal) {
			ret = -EBUSY;
			goto out;
		}
		(void)tty_detach_session_locked(tty, displaced, &released);
	}
	attachment = tty_find_attachment_locked(tty, task_pid(leader));
	if (!attachment) {
		fresh->leader_pid = task_pid(leader);
		fresh->session = sid;
		list_add_tail(&fresh->node, &tty->attachments);
		attachment = fresh;
		fresh = NULL;
	}
	attachment->session = sid;
	tty->session = sid;
	tty->foreground_pgid = pgid;
out:
	mutex_unlock(&tty->lock);
	tty_free_attachments(&released);
	kfree(fresh);
	return ret;
}

bool tty_ctty_owned_by(struct tty_endpoint *tty, struct task_struct *task,
		       pid_t sid)
{
	bool owned;

	if (!tty)
		return false;
	mutex_lock(&tty->lock);
	owned = tty_ctty_owned_by_locked(tty, task, sid);
	mutex_unlock(&tty->lock);
	return owned;
}

int tty_ctty_get_foreground_pgid(struct tty_endpoint *tty,
				 struct task_struct *task, pid_t sid,
				 pid_t *pgid)
{
	int ret = 0;

	if (!tty || !pgid)
		return -EINVAL;

	mutex_lock(&tty->lock);
	if (!tty_ctty_owned_by_locked(tty, task, sid))
		ret = -ENOTTY;
	else
		*pgid = tty->foreground_pgid;
	mutex_unlock(&tty->lock);
	return ret;
}

int tty_ctty_set_foreground_pgid(struct tty_endpoint *tty,
				 struct task_struct *task, pid_t sid,
				 pid_t pgid)
{
	int ret = 0;

	if (!tty)
		return -EINVAL;

	mutex_lock(&tty->lock);
	if (!tty_ctty_owned_by_locked(tty, task, sid))
		ret = -ENOTTY;
	else
		tty->foreground_pgid = pgid;
	mutex_unlock(&tty->lock);
	return ret;
}

int tty_ctty_snapshot_foreground(struct tty_endpoint *tty,
				 struct tty_ctty_state *state)
{
	int ret = 0;

	if (!tty || !state)
		return -EINVAL;

	mutex_lock(&tty->lock);
	if (!tty_has_owner_locked(tty))
		ret = -ESRCH;
	else
		tty_ctty_state_set_locked(tty, state);
	mutex_unlock(&tty->lock);
	return ret;
}

void tty_ctty_clear_foreground_if(pid_t sid, pid_t pgid)
{
	mutex_lock(&console_tty.lock);
	if (console_tty.session == sid && console_tty.foreground_pgid == pgid)
		console_tty.foreground_pgid = 0;
	mutex_unlock(&console_tty.lock);
}

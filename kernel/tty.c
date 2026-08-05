/*
 * kernel/tty.c - single-console terminal state and process attachments
 */

#include <kernel/errno.h>
#include <kernel/list.h>
#include <kernel/slab.h>
#include <kernel/mutex.h>
#include <kernel/proc.h>
#include <kernel/tty.h>

#include "tty_internal.h"

struct tty_endpoint {
	mutex_t lock;
	struct session_struct *session;
	struct pgrp_struct *foreground;
	struct list_head attachments;
};

struct tty_process_attachment {
	pid_t leader_pid;
	pid_t session;
	struct list_head node;
};

static struct tty_endpoint console_tty = {
	.lock = MUTEX_INIT(console_tty.lock, LOCK_RANK_TTY),
	.attachments = LIST_HEAD_INIT(console_tty.attachments),
};

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
	state->session = NULL;
	state->foreground = NULL;
}

static void tty_ctty_state_set_locked(struct tty_endpoint *tty,
				      struct tty_ctty_state *state)
{
	if (!state)
		return;
	state->session = tty->session;
	state->foreground = tty->foreground;
	proc_session_get(state->session);
	proc_pgrp_get(state->foreground);
}

static void tty_ctty_state_take_owner_locked(struct tty_endpoint *tty,
						struct tty_ctty_state *state)
{
	if (!state)
		return;
	state->session = tty->session;
	/* Never overwrite a foreground already transferred by
	 * tty_ctty_clear_foreground_if() in the same detached state. */
	if (!state->foreground)
		state->foreground = tty->foreground;
	tty->session = NULL;
	tty->foreground = NULL;
	if (state->session && state->session->ctty == tty)
		state->session->ctty = NULL;
}

void tty_ctty_state_release(struct tty_ctty_state *state)
{
	if (!state)
		return;
	proc_pgrp_put(state->foreground);
	proc_session_put(state->session);
	tty_ctty_state_clear(state);
}

static bool tty_has_owner_locked(const struct tty_endpoint *tty)
{
	return tty->session != NULL;
}

static bool tty_ctty_owned_by_locked(struct tty_endpoint *tty,
				     struct proc_struct *proc, pid_t sid)
{
	struct tty_process_attachment *attachment;

	if (!proc || !proc->pid || sid <= 0 || !tty_has_owner_locked(tty) ||
	    !tty->session->sid || tty->session->sid->nr != sid)
		return false;
	attachment = tty_find_attachment_locked(tty, proc->pid->nr);
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
	tty_ctty_state_take_owner_locked(tty, detached);
	list_for_each_safe (pos, next, &tty->attachments) {
		struct tty_process_attachment *attachment =
			list_entry(pos, struct tty_process_attachment, node);

		tty_move_attachment_locked(attachment, released);
	}
	return true;
}

void tty_console_endpoint_init(void)
{
	mutex_init(&console_tty.lock, LOCK_RANK_TTY);
	console_tty.session = NULL;
	console_tty.foreground = NULL;
	INIT_LIST_HEAD(&console_tty.attachments);
}

struct tty_endpoint *tty_console_endpoint(void)
{
	return &console_tty;
}

int tty_ctty_clone_attachment(struct proc_struct *parent,
			      struct proc_struct *child)
{
	struct tty_process_attachment *parent_attachment;
	struct tty_process_attachment *child_attachment;
	pid_t session;

	if (!parent || !child || !parent->pid || !child->pid)
		return -ESRCH;

	mutex_lock(&console_tty.lock);
	parent_attachment =
		tty_find_attachment_locked(&console_tty, parent->pid->nr);
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
	parent_attachment =
		tty_find_attachment_locked(&console_tty, parent->pid->nr);
	if (parent_attachment && parent_attachment->session == session) {
		BUG_ON(tty_find_attachment_locked(&console_tty,
						  child->pid->nr));
		child_attachment->leader_pid = child->pid->nr;
		child_attachment->session = session;
		list_add_tail(&child_attachment->node,
			      &console_tty.attachments);
		child_attachment = NULL;
	}
	mutex_unlock(&console_tty.lock);

	kfree(child_attachment);
	return 0;
}

void tty_ctty_remove_proc(struct proc_struct *proc, pid_t sid,
			  enum tty_ctty_remove_scope scope,
			  struct tty_ctty_state *detached)
{
	LIST_HEAD(released);
	struct tty_ctty_state discarded = {0};
	struct tty_ctty_state *removed = detached ? detached : &discarded;
	struct tty_process_attachment *attachment;

	tty_ctty_state_clear(removed);
	if (!proc || !proc->pid)
		return;

	mutex_lock(&console_tty.lock);
	attachment = tty_find_attachment_locked(&console_tty, proc->pid->nr);
	if (scope == TTY_CTTY_REVOKE_SESSION && console_tty.session &&
	    console_tty.session->sid && console_tty.session->sid->nr == sid) {
		(void)tty_detach_session_locked(&console_tty, removed,
						&released);
	} else if (attachment && attachment->session == sid) {
		tty_move_attachment_locked(attachment, &released);
	}
	mutex_unlock(&console_tty.lock);
	tty_free_attachments(&released);
	if (!detached)
		tty_ctty_state_release(&discarded);
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

int tty_ctty_claim(struct tty_endpoint *tty, struct proc_struct *proc,
		   pid_t sid, pid_t pgid, bool steal,
		   struct tty_ctty_state *displaced)
{
	LIST_HEAD(released);
	struct tty_process_attachment *attachment;
	struct tty_process_attachment *fresh;
	struct session_struct *session;
	struct pgrp_struct *foreground;
	struct pgrp_struct *old_foreground = NULL;
	struct tty_ctty_state discarded = {0};
	struct tty_ctty_state *displaced_state = displaced ? displaced : &discarded;
	int ret = 0;

	if (!tty || !proc || !proc->pid || sid <= 0 || pgid <= 0)
		return -EINVAL;
	session = proc_lookup_session(sid);
	foreground = proc_lookup_pgrp(pgid);
	if (!session || !foreground || foreground->session != session) {
		proc_pgrp_put(foreground);
		proc_session_put(session);
		return -ESRCH;
	}

	fresh = kzalloc(sizeof(*fresh), ALLOC_NOWAIT);
	if (!fresh) {
		proc_pgrp_put(foreground);
		proc_session_put(session);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&fresh->node);
	tty_ctty_state_clear(displaced_state);

	mutex_lock(&tty->lock);
	if (tty_has_owner_locked(tty) &&
	    (!tty->session->sid || tty->session->sid->nr != sid)) {
		if (!steal) {
			ret = -EBUSY;
			goto out;
		}
		(void)tty_detach_session_locked(tty, displaced_state, &released);
	}
	if (!tty_has_owner_locked(tty)) {
		tty->session = session;
		tty->foreground = foreground;
		session->ctty = tty;
		session = NULL;
		foreground = NULL;
	} else if (tty->session == session) {
		old_foreground = tty->foreground;
		tty->foreground = foreground;
		foreground = NULL;
	} else {
		ret = -EINVAL;
		goto out;
	}
	attachment = tty_find_attachment_locked(tty, proc->pid->nr);
	if (!attachment) {
		fresh->leader_pid = proc->pid->nr;
		fresh->session = sid;
		list_add_tail(&fresh->node, &tty->attachments);
		attachment = fresh;
		fresh = NULL;
	}
	attachment->session = sid;
out:
	mutex_unlock(&tty->lock);
	tty_free_attachments(&released);
	kfree(fresh);
	proc_pgrp_put(old_foreground);
	proc_pgrp_put(foreground);
	proc_session_put(session);
	if (!displaced)
		tty_ctty_state_release(&discarded);
	return ret;
}

bool tty_ctty_owned_by(struct tty_endpoint *tty, struct proc_struct *proc,
		       pid_t sid)
{
	bool owned;

	if (!tty)
		return false;
	mutex_lock(&tty->lock);
	owned = tty_ctty_owned_by_locked(tty, proc, sid);
	mutex_unlock(&tty->lock);
	return owned;
}

int tty_ctty_get_foreground_pgid(struct tty_endpoint *tty,
				 struct proc_struct *proc, pid_t sid,
				 pid_t *pgid)
{
	int ret = 0;

	if (!tty || !pgid)
		return -EINVAL;

	mutex_lock(&tty->lock);
	if (!tty_ctty_owned_by_locked(tty, proc, sid))
		ret = -ENOTTY;
	else
		*pgid = tty->foreground && tty->foreground->pgid
			? tty->foreground->pgid->nr : 0;
	mutex_unlock(&tty->lock);
	return ret;
}

int tty_ctty_set_foreground_pgid(struct tty_endpoint *tty,
				 struct proc_struct *proc, pid_t sid,
				 pid_t pgid)
{
	struct pgrp_struct *foreground;
	struct pgrp_struct *old_foreground = NULL;
	int ret = 0;

	if (!tty)
		return -EINVAL;
	foreground = proc_lookup_pgrp(pgid);
	if (!foreground)
		return -ESRCH;

	mutex_lock(&tty->lock);
	if (!tty_ctty_owned_by_locked(tty, proc, sid))
		ret = -ENOTTY;
	else if (!tty->session || foreground->session != tty->session)
		ret = -EPERM;
	else {
		old_foreground = tty->foreground;
		tty->foreground = foreground;
		foreground = NULL;
	}
	mutex_unlock(&tty->lock);
	proc_pgrp_put(old_foreground);
	proc_pgrp_put(foreground);
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

void tty_ctty_clear_foreground_if(pid_t sid, pid_t pgid,
				  struct tty_ctty_state *detached)
{
	mutex_lock(&console_tty.lock);
	if (console_tty.session && console_tty.session->sid &&
	    console_tty.session->sid->nr == sid && console_tty.foreground &&
	    console_tty.foreground->pgid &&
	    console_tty.foreground->pgid->nr == pgid) {
		if (detached)
			detached->foreground = console_tty.foreground;
		console_tty.foreground = NULL;
	}
	mutex_unlock(&console_tty.lock);
}

void tty_ctty_release_owner_if(pid_t sid, const struct proc_struct *exclude,
			       struct tty_ctty_state *detached)
{
	LIST_HEAD(released);

	mutex_lock(&console_tty.lock);
	if (!console_tty.session || !console_tty.session->sid ||
	    console_tty.session->sid->nr != sid) {
		mutex_unlock(&console_tty.lock);
		return;
	}
	/* Session emptiness is topology-owned; the check runs under the
	 * topology lock (20), which orders after session (10) and TTY (11). */
	if (!proc_session_is_empty(console_tty.session, exclude)) {
		mutex_unlock(&console_tty.lock);
		return;
	}
	(void)tty_detach_session_locked(&console_tty, detached, &released);
	mutex_unlock(&console_tty.lock);
	tty_free_attachments(&released);
}

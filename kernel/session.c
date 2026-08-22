/*
 * kernel/session.c - process-session and controlling-TTY coordination
 */

#include <nuvix/errno.h>
#include <nuvix/pid.h>
#include <nuvix/proc.h>
#include <nuvix/session.h>
#include <nuvix/signal.h>
#include <nuvix/mutex.h>
#include <nuvix/task.h>
#include <nuvix/tools.h>
#include <uapi/signal.h>

#include "tty_internal.h"

static DEFINE_MUTEX(session_lock, LOCK_RANK_SESSION, LOCK_IRQ_TASK_ONLY);

struct session_process_identity {
	pid_t pgid;
	pid_t sid;
};

static int session_send_foreground_signal(int sig,
					  struct pgrp_struct *foreground,
					  struct session_struct *session)
{
	siginfo_t info = {0};

	info.si_signo = sig;
	info.si_code = SI_KERNEL;
	return sig_send_pgrp(sig, &info, foreground, session);
}

static void session_signal_hangup(struct tty_ctty_state *detached)
{
	if (!detached || !detached->session || !detached->foreground) {
		tty_ctty_state_release(detached);
		return;
	}
	(void)session_send_foreground_signal(SIGHUP, detached->foreground,
					     detached->session);
	(void)session_send_foreground_signal(SIGCONT, detached->foreground,
					     detached->session);
	tty_ctty_state_release(detached);
}

static void session_clear_empty(pid_t sid, pid_t pgid,
				const struct task_struct *exclude,
				struct tty_ctty_state *detached)
{
	if (sid <= 0 || pgid <= 0 || !detached)
		return;
	if (proc_pgrp_has_member(pgid, sid, exclude))
		return;
	tty_ctty_clear_foreground_if(sid, pgid, detached);
	/* An empty session must not stay attached to the TTY: release the
	 * owner reference so a future claim is not blocked forever.  The
	 * exiting last member is excluded from the emptiness check: its pgrp
	 * unlink only happens later in the reaper. */
	tty_ctty_release_owner_if(sid, exclude ? exclude->proc : NULL,
				  detached);
}

int session_process_clone_prepare(struct task_struct *child,
				  struct task_struct *parent,
				  bool share_thread_group)
{
	pid_t pgid;
	pid_t sid;
	int ret;

	if (!child || !parent)
		return -EINVAL;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(parent->proc, &pgid, &sid);
	if (ret == 0)
		ret = proc_join_pgrp(child->proc, pgid, NULL);
	if (ret == 0 && !share_thread_group)
		ret = tty_ctty_clone_attachment(parent->proc, child->proc);
	mutex_unlock(&session_lock);
	return ret;
}

int session_process_setsid(struct task_struct *task)
{
	struct proc_struct *proc;
	struct session_process_identity old_identity;
	struct proc_orphan_event orphan_events[8];
	struct tty_ctty_state detached = {0};
	size_t orphan_count = 0;
	pid_t new_sid;
	int ret;

	if (!task || !task->proc)
		return -ESRCH;
	proc = task->proc;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &old_identity.pgid,
				     &old_identity.sid);
	if (ret == 0)
		ret = proc_create_session(proc, &new_sid, orphan_events,
					  ARRLEN(orphan_events), &orphan_count);
	if (ret > 0) {
		tty_ctty_remove_proc(proc, old_identity.sid,
				     TTY_CTTY_REMOVE_TASK, &detached);
		session_clear_empty(old_identity.sid, old_identity.pgid, NULL,
				    &detached);
	}
	mutex_unlock(&session_lock);
	for (size_t index = 0; index < orphan_count; index++)
		sig_orphan_pgrp(&orphan_events[index]);
	for (size_t index = 0; index < orphan_count; index++)
		proc_orphan_event_release(&orphan_events[index]);
	tty_ctty_state_release(&detached);
	return ret;
}

int session_process_setpgid(pid_t pid, pid_t pgid)
{
	struct task_struct *caller = current_task();
	struct task_struct *leader = NULL;
	struct proc_struct *target;
	struct proc_struct *parent = NULL;
	struct session_process_identity old_identity;
	struct proc_orphan_event orphan = {0};
	struct tty_ctty_state detached = {0};
	bool put_target = false;
	int ret;

	if (!caller || !caller->proc)
		return -EINVAL;
	if (pid == 0)
		target = caller->proc;
	else {
		target = pid_lookup_proc(pid);
		put_target = true;
	}
	if (!target)
		return -ESRCH;

	mutex_lock(&session_lock);
	leader = proc_leader_get(target);
	if (!leader) {
		ret = -ESRCH;
		goto out_locked;
	}
	if (pid == 0 && !proc_task_is_leader(target, caller)) {
		ret = -ESRCH;
		goto out_locked;
	}
	parent = proc_parent_get(target);
	if (leader != caller && parent != caller->proc) {
		ret = -EPERM;
		goto out_locked;
	}
	ret = proc_snapshot_topology(target, &old_identity.pgid,
				     &old_identity.sid);
	if (ret < 0)
		goto out_locked;
	if (pgid == 0)
		pgid = target->pid->nr;
	if (pgid != target->pid->nr &&
	    !proc_pgrp_has_member(pgid, old_identity.sid, NULL)) {
		ret = -EPERM;
		goto out_locked;
	}
	ret = proc_join_pgrp(target, pgid, &orphan);
	if (ret == 0)
		session_clear_empty(old_identity.sid, old_identity.pgid, NULL,
				    &detached);
out_locked:
	mutex_unlock(&session_lock);
	task_put(leader);
	proc_put(parent);
	if (put_target)
		proc_put(target);
	if (orphan.pgid > 0)
		sig_orphan_pgrp(&orphan);
	proc_orphan_event_release(&orphan);
	tty_ctty_state_release(&detached);
	return ret;
}

static int session_process_get_identity(pid_t pid, bool sid)
{
	struct proc_struct *proc;
	struct task_struct *leader = NULL;
	bool put_proc;
	pid_t pgid;
	pid_t session_id;
	int ret;

	if (pid < 0)
		return -ESRCH;
	proc = pid == 0 ? current_task()->proc : pid_lookup_proc(pid);
	put_proc = pid != 0;
	if (!proc)
		return -ESRCH;
	if (put_proc) {
		leader = proc_leader_get(proc);
		if (!leader) {
			/* Exited process awaiting wait4: identity was snapshotted
			 * at exit and must stay queryable until the proc is
			 * reaped and unpublished. */
			bool have_exit_identity =
				proc_exit_identity(proc, &pgid, &session_id);

			proc_put(proc);
			if (!have_exit_identity)
				return -ESRCH;
			return sid ? session_id : pgid;
		}
	}
	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &pgid, &session_id);
	mutex_unlock(&session_lock);
	task_put(leader);
	if (put_proc)
		proc_put(proc);
	if (ret < 0)
		return ret;
	return sid ? session_id : pgid;
}

int session_process_getpgid(pid_t pid)
{
	return session_process_get_identity(pid, false);
}

int session_process_getsid(pid_t pid)
{
	return session_process_get_identity(pid, true);
}

static void session_process_cleanup(struct task_struct *task)
{
	struct session_process_identity identity;
	struct tty_ctty_state detached = {0};

	if (!task || !task->proc)
		return;
	/* Session/TTY cleanup belongs to the dying process, not to the group
	 * leader: a leader exit with live siblings must keep the attachment
	 * and the foreground pgrp. Only the last task of the proc detaches. */
	if (!proc_is_last_task(task->proc, task))
		return;

	mutex_lock(&session_lock);
	if (proc_snapshot_topology(task->proc, &identity.pgid, &identity.sid) ==
	    0) {
		tty_ctty_remove_proc(task->proc, identity.sid,
				     identity.sid == task->proc->pid->nr
					     ? TTY_CTTY_REVOKE_SESSION
					     : TTY_CTTY_REMOVE_TASK,
				     &detached);
		if (!detached.session)
			session_clear_empty(identity.sid, identity.pgid, task,
					    &detached);
	} else {
		tty_ctty_state_release(&detached);
	}
	mutex_unlock(&session_lock);
	session_signal_hangup(&detached);
}

void session_process_exit(struct task_struct *task)
{
	session_process_cleanup(task);
}

/*
 * Reaper-side session recheck: proc_publish_exit() has just unlinked the
 * exiting proc from its pgrp, so a session whose last member just left is
 * now visibly empty.  The exit-time path cannot see this (the member is
 * still linked then), and two last members of the same session exiting in
 * the same reaper interval would both miss the check there; the reaper
 * serializes the unlinks, so this check is race-free.
 */
void session_process_reaper(struct proc_struct *proc)
{
	struct session_struct *session = proc_session_ref_of(proc);
	struct tty_ctty_state detached = {0};

	if (!session)
		return;
	if (proc_session_is_empty(session, NULL)) {
		pid_t sid = session->sid ? session->sid->nr : 0;

		mutex_lock(&session_lock);
		tty_ctty_release_owner_if(sid, NULL, &detached);
		mutex_unlock(&session_lock);
	}
	proc_session_put(session);
	tty_ctty_state_release(&detached);
}

void session_process_abort(struct task_struct *task)
{
	session_process_cleanup(task);
}

int session_console_acquire(int steal)
{
	struct proc_struct *proc = current_task()->proc;
	struct tty_endpoint *tty = tty_console_endpoint();
	struct session_process_identity identity;
	struct tty_ctty_state owner = {0};
	struct tty_ctty_state displaced = {0};
	bool has_owner;
	int ret;

	if (!proc)
		return -ESRCH;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &identity.pgid, &identity.sid);
	if (ret < 0)
		goto out;
	if (identity.sid != proc->pid->nr) {
		ret = -EPERM;
		goto out;
	}
	has_owner = tty_ctty_has_owner(tty, &owner);
	if (has_owner &&
	    (!owner.session || !owner.session->sid ||
	     owner.session->sid->nr != identity.sid) &&
	    (steal != 1 || task_uid(current_task()) != 0)) {
		ret = -EPERM;
		goto out;
	}
	ret = tty_ctty_claim(tty, proc, identity.sid, identity.pgid,
			     has_owner && owner.session && owner.session->sid &&
				     owner.session->sid->nr != identity.sid,
			     &displaced);
out:
	mutex_unlock(&session_lock);
	tty_ctty_state_release(&owner);
	if (ret == 0 || displaced.session)
		session_signal_hangup(&displaced);
	else
		tty_ctty_state_release(&displaced);
	return ret;
}

int session_console_release(void)
{
	struct proc_struct *proc = current_task()->proc;
	struct tty_endpoint *tty = tty_console_endpoint();
	struct session_process_identity identity;
	struct tty_ctty_state detached = {0};
	int ret;

	if (!proc)
		return -ESRCH;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &identity.pgid, &identity.sid);
	if (ret < 0)
		goto out;
	if (!tty_ctty_owned_by(tty, proc, identity.sid)) {
		ret = -ENOTTY;
		goto out;
	}
	tty_ctty_remove_proc(proc, identity.sid,
			     identity.sid == proc->pid->nr
				     ? TTY_CTTY_REVOKE_SESSION
				     : TTY_CTTY_REMOVE_TASK,
			     &detached);
	ret = 0;
out:
	mutex_unlock(&session_lock);
	if (ret == 0)
		session_signal_hangup(&detached);
	else
		tty_ctty_state_release(&detached);
	return ret;
}

int session_console_get_foreground_pgid(pid_t *pgid)
{
	struct proc_struct *proc = current_task()->proc;
	struct tty_endpoint *tty = tty_console_endpoint();
	struct session_process_identity identity;
	int ret;

	if (!proc)
		return -ESRCH;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &identity.pgid, &identity.sid);
	if (ret == 0)
		ret = tty_ctty_get_foreground_pgid(tty, proc, identity.sid,
						   pgid);
	mutex_unlock(&session_lock);
	return ret;
}

int session_console_set_foreground_pgid(pid_t pgid)
{
	struct proc_struct *proc = current_task()->proc;
	struct tty_endpoint *tty = tty_console_endpoint();
	struct session_process_identity identity;
	int ret;

	if (!proc)
		return -ESRCH;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &identity.pgid, &identity.sid);
	if (ret < 0)
		goto out;
	if (!tty_ctty_owned_by(tty, proc, identity.sid)) {
		ret = -ENOTTY;
		goto out;
	}
	if (pgid <= 0) {
		ret = -EINVAL;
		goto out;
	}
	if (!proc_pgrp_has_member(pgid, identity.sid, NULL)) {
		ret = -EPERM;
		goto out;
	}
	ret = tty_ctty_set_foreground_pgid(tty, proc, identity.sid, pgid);
out:
	mutex_unlock(&session_lock);
	return ret;
}

int session_console_get_sid(pid_t *sid)
{
	struct proc_struct *proc = current_task()->proc;
	struct tty_endpoint *tty = tty_console_endpoint();
	struct session_process_identity identity;
	int ret;

	if (!proc)
		return -ESRCH;
	if (!sid)
		return -EINVAL;

	mutex_lock(&session_lock);
	ret = proc_snapshot_topology(proc, &identity.pgid, &identity.sid);
	if (ret == 0 && !tty_ctty_owned_by(tty, proc, identity.sid))
		ret = -ENOTTY;
	if (ret == 0)
		*sid = identity.sid;
	mutex_unlock(&session_lock);
	return ret;
}

int session_console_deliver_foreground_signal(int sig)
{
	struct tty_endpoint *tty = tty_console_endpoint();
	struct tty_ctty_state foreground = {0};
	int ret;

	if (!sig_valid(sig))
		return -EINVAL;

	mutex_lock(&session_lock);
	ret = tty_ctty_snapshot_foreground(tty, &foreground);
	mutex_unlock(&session_lock);
	if (ret < 0)
		return ret;
	if (!foreground.session || !foreground.foreground)
		ret = -ESRCH;
	else
		ret = session_send_foreground_signal(sig, foreground.foreground,
						     foreground.session);
	tty_ctty_state_release(&foreground);
	return ret;
}

#ifndef _NUVIX_SESSION_H
#define _NUVIX_SESSION_H

/*
 * include/nuvix/session.h - process session and controlling-TTY policy
 *
 * Locking: every exported entry point serializes on the module-private
 * session mutex (LOCK_RANK_SESSION, 10).  Callers must not hold any other
 * lock when calling in; the module acquires session -> TTY (11) ->
 * topology/PID (20) in increasing rank.  Reference moves: the module only
 * detaches TTY/pgrp/session state inside the lock; released references are
 * returned to the caller through tty_ctty_state snapshots and must be
 * dropped with tty_ctty_state_release() / proc_orphan_event_release()
 * outside the session lock.  No final put, signal, wake, or hangup runs
 * while the session lock is held.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>

struct task_struct;
struct proc_struct;

__must_check
int session_process_clone_prepare(struct task_struct *child,
		struct task_struct *parent, bool share_thread_group);

__must_check
int session_process_getpgid(pid_t pid);

__must_check
int session_process_getsid(pid_t pid);

__must_check
int session_process_setsid(struct task_struct *task);

__must_check
int session_process_setpgid(pid_t pid, pid_t pgid);

void session_process_exit(struct task_struct *task);
/**
 * @brief Reaper-side session emptiness recheck.
 * @param proc Exiting proc whose pgrp unlink was just published.
 *
 * Called after proc_publish_exit() so a session whose last member just
 * left releases its controlling-TTY owner reference.
 */
void session_process_reaper(struct proc_struct *proc);

void session_process_abort(struct task_struct *task);

__must_check
int session_console_acquire(int steal);

__must_check
int session_console_release(void);

__must_check
int session_console_get_foreground_pgid(pid_t *pgid);

__must_check
int session_console_set_foreground_pgid(pid_t pgid);

__must_check
int session_console_get_sid(pid_t *sid);

int session_console_deliver_foreground_signal(int sig);

#endif

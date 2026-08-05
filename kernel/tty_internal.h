#ifndef _CUTEOS_KERNEL_TTY_INTERNAL_H
#define _CUTEOS_KERNEL_TTY_INTERNAL_H

#include <kernel/compiler.h>
#include <kernel/types.h>

struct tty_endpoint;
struct pgrp_struct;
struct proc_struct;
struct session_struct;

struct tty_ctty_state {
	struct session_struct *session;
	struct pgrp_struct *foreground;
};

enum tty_ctty_remove_scope {
	TTY_CTTY_REMOVE_TASK,
	TTY_CTTY_REVOKE_SESSION,
};

void tty_console_endpoint_init(void);
struct tty_endpoint *tty_console_endpoint(void) __must_check;
int tty_ctty_clone_attachment(struct proc_struct *parent,
			      struct proc_struct *child) __must_check;
void tty_ctty_remove_proc(struct proc_struct *proc, pid_t sid,
			  enum tty_ctty_remove_scope scope,
			  struct tty_ctty_state *detached);
void tty_ctty_state_release(struct tty_ctty_state *state);
bool tty_ctty_has_owner(struct tty_endpoint *tty, struct tty_ctty_state *state);
int tty_ctty_claim(struct tty_endpoint *tty, struct proc_struct *proc,
		   pid_t sid, pid_t pgid, bool steal,
		   struct tty_ctty_state *displaced) __must_check;
bool tty_ctty_owned_by(struct tty_endpoint *tty, struct proc_struct *proc,
		       pid_t sid);
int tty_ctty_get_foreground_pgid(struct tty_endpoint *tty,
				 struct proc_struct *proc, pid_t sid,
				 pid_t *pgid) __must_check;
int tty_ctty_set_foreground_pgid(struct tty_endpoint *tty,
				 struct proc_struct *proc, pid_t sid,
				 pid_t pgid) __must_check;
int tty_ctty_snapshot_foreground(struct tty_endpoint *tty,
				 struct tty_ctty_state *state) __must_check;

/*
 * Clear the TTY foreground pgrp when it still matches (sid, pgid).  The
 * removed pgrp reference is transferred into @p detached, which the caller
 * must release with tty_ctty_state_release() outside the session lock.
 */
void tty_ctty_clear_foreground_if(pid_t sid, pid_t pgid,
				  struct tty_ctty_state *detached);

/*
 * Detach the TTY owner when @p sid matches and the session has no pgrp with
 * remaining members.  The owner references are transferred into @p detached
 * for release outside the session lock; a live session is left untouched.
 * @p exclude (typically the exiting last member) is treated as absent by
 * the emptiness check.
 */
void tty_ctty_release_owner_if(pid_t sid, const struct proc_struct *exclude,
			       struct tty_ctty_state *detached);

#endif

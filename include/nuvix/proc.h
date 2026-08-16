#ifndef _NUVIX_PROC_H
#define _NUVIX_PROC_H

/**
 * @file proc.h
 * @brief Process-owned resources, topology, and wait-visible lifecycle.
 *
 * A proc is the owner of everything that describes a POSIX process as a
 * system entity.  A task only owns execution-context state and points at its
 * proc.  The lists in this header are structural membership; references are
 * acquired explicitly when an object crosses an owner lock.
 *
 * Locking: topology membership and proc lifecycle are guarded by the
 * module-private proc_topology_lock (LOCK_RANK_TOPOLOGY, 60), acquired
 * inside session (10) / TTY (20) and before the proc wait state lock (310).
 * Lock-held sections detach objects and move references only; release every
 * acquired reference (proc/pgrp/session/task/pid) outside the lock.  The
 * two-phase wait claim (proc_wait_claim/commit/abort) hands out strong
 * parent/child references; the caller must commit or abort exactly once and
 * drop the references outside any lock.  pgrp/session snapshot events
 * (proc_orphan_event, proc_parent_event) carry their own references and are
 * released with the matching release function outside locks.
 */

#include <nuvix/cputime.h>
#include <nuvix/list.h>
#include <nuvix/pid.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>
#include <nuvix/wait.h>
#include <nuvix/types.h>
#include <uapi/resource.h>

struct files_struct;
struct fs_struct;
struct mm_struct;
struct pid_identity;
struct sighand_struct;
struct signal_struct;
struct task_struct;
struct tty_endpoint;

struct pgrp_struct;
struct session_struct;

enum proc_lifecycle {
	PROC_NEW,
	PROC_LIVE,
	PROC_EXITING,
	PROC_ZOMBIE,
	PROC_DEAD,
};

enum proc_wait_event {
	PROC_WAIT_EXIT = 1u << 0,
	PROC_WAIT_STOP = 1u << 1,
	PROC_WAIT_CONTINUE = 1u << 2,
};

/**
 * @struct proc_wait_state
 * @brief Persistent process events consumed by wait4 in two phases.
 *
 * Exit is a durable event. Stop and continue are edge events latched until a
 * successful wait claim is committed. No heap event record is used, so a
 * failed userspace copy can only release a claim and cannot lose an event.
 */
struct proc_wait_state {
	spinlock_t lock;
	struct wait_channel channel;
	uint64_t generation;
	uint64_t exit_generation;
	uint64_t stop_generation;
	uint64_t continue_generation;
	uint64_t claimed_exit_generation;
	uint64_t claimed_stop_generation;
	uint64_t claimed_continue_generation;
	uint32_t pending;
	int exit_status;
	int stop_status;
	int continue_status;
	int exit_signal;
	pid_t creator_tid;
	uid_t exit_uid;
};

struct proc_vfork_state {
	spinlock_t lock;
	struct wait_channel channel;
	bool active;
	bool completed;
};

/**
 * @struct pgrp_struct
 * @brief Process-group object retaining its PGID identity and session.
 */
struct pgrp_struct {
	refcount_t refs;
	spinlock_t lock;
	struct pid_identity *pgid;
	struct session_struct *session;
	struct list_head members;
	struct list_head session_node;
	bool orphaned;
};

/**
 * @struct session_struct
 * @brief Session object retained by process groups and controlling TTYs.
 */
struct session_struct {
	refcount_t refs;
	spinlock_t lock;
	struct pid_identity *sid;
	struct list_head pgrps;
	struct tty_endpoint *ctty;
};

/**
 * @struct proc_struct
 * @brief Process object and the sole owner of process-wide state.
 */
struct proc_struct {
	refcount_t refs;
	spinlock_t lock;
	struct pid_identity *pid;
	enum proc_lifecycle lifecycle;
	bool published;

	struct mm_struct *mm;
	struct files_struct *files;
	struct fs_struct *fs;
	struct sighand_struct *sighand;
	struct signal_struct *signal;
	struct rlimit64 rlimits[RLIM_NLIMITS];

	struct list_head tasks;
	uint32_t nr_tasks;
	struct task_struct *leader;
	bool job_stopped;

	struct proc_struct *parent;
	struct list_head children;
	struct list_head sibling;
	struct pgrp_struct *pgrp;
	struct list_head pgrp_node;

	struct proc_wait_state wait_state;
	struct proc_vfork_state vfork;
	struct wait_channel exec_channel;
	struct task_struct *exec_owner;
	struct task_cputime cputime;
	struct task_cputime children_cputime;
	int group_exit_status;
	bool group_exit_status_valid;
	bool group_exit_requested;
	int exit_status;
	uid_t exit_uid;
	pid_t exit_pgid;
	pid_t exit_sid;
	bool exit_pending;
	bool exit_auto_reap;
	bool exit_sigchld_notify;
	bool user_process;
};

struct proc_cputime_snapshot {
	struct task_cputime self;
	struct task_cputime children;
};

/** Process construction/publication and references. */
__must_check
struct proc_struct *proc_alloc(struct proc_struct *parent, struct pid_identity *pid);

void proc_get(struct proc_struct *proc);

void proc_put(struct proc_struct *proc);

__must_check
bool proc_try_get(struct proc_struct *proc);

/** Topology-owned references for objects crossing the topology lock. */
void proc_pgrp_get(struct pgrp_struct *pgrp);

bool proc_pgrp_try_get(struct pgrp_struct *pgrp);

void proc_pgrp_put(struct pgrp_struct *pgrp);

void proc_session_get(struct session_struct *session);
/**
 * @brief Take a reference on the session of @p proc's pgrp.
 *
 * The pointer snapshot is taken under the topology lock; the caller keeps
 * the returned reference (may be NULL) until proc_session_put().  Intended
 * for the reaper, where the proc is exiting and its pgrp attachment cannot
 * change.
 */
struct session_struct *proc_session_ref_of(const struct proc_struct *proc);

bool proc_session_try_get(struct session_struct *session);

void proc_session_put(struct session_struct *session);

__must_check
struct pgrp_struct *proc_lookup_pgrp(pid_t pgid);

__must_check
struct session_struct *proc_lookup_session(pid_t sid);

int proc_init_resources(struct proc_struct *proc);

int proc_clone_rlimits(struct proc_struct *proc, struct proc_struct *source);

void proc_release_resources(struct proc_struct *proc);

/** Replace a proc-owned resource and return its previous reference. */
struct mm_struct *proc_replace_mm(struct proc_struct *proc, struct mm_struct *mm);

struct files_struct *proc_replace_files(struct proc_struct *proc,
					struct files_struct *files);

struct fs_struct *proc_replace_fs(struct proc_struct *proc,
					struct fs_struct *fs);

int proc_publish(struct proc_struct *proc);

int proc_publish_with_task(struct proc_struct *proc, struct task_struct *task);

void proc_unpublish(struct proc_struct *proc);

/** Process-owned accounting updates and lock-consistent snapshots. */
void proc_account_task_cputime(struct proc_struct *proc, const struct task_cputime *time);

void proc_account_child_cputime(struct proc_struct *proc, const struct task_cputime *time);

void proc_cputime_snapshot(struct proc_struct *proc, struct proc_cputime_snapshot *snapshot);

/** Task membership and process topology. */
int proc_attach_task(struct proc_struct *proc, struct task_struct *task, bool leader);

bool proc_is_last_task(const struct proc_struct *proc, const struct task_struct *task);

bool proc_detach_task(struct proc_struct *proc, struct task_struct *task);

/** Lock-consistent task membership queries owned by the proc module. */
__must_check
bool proc_task_is_member(const struct proc_struct *proc,
			 const struct task_struct *task);

__must_check
bool proc_task_is_leader(const struct proc_struct *proc,
			 const struct task_struct *task);

/** Process role state owned by proc, including exec/fork transitions. */
__must_check
bool proc_is_user_process(struct proc_struct *proc);

void proc_mark_user_process(struct proc_struct *proc);

void proc_inherit_user_process(struct proc_struct *child,
			       struct proc_struct *parent);

int proc_link_child(struct proc_struct *parent, struct proc_struct *child);

struct proc_orphan_event {
	pid_t pgid;
	pid_t sid;
	struct pgrp_struct *pgrp;
	struct session_struct *session;
};

void proc_orphan_event_release(struct proc_orphan_event *event);

/** Parent notification snapshot held until proc_parent_event_release(). */
struct proc_parent_event {
	struct proc_struct *parent;
	struct proc_struct *child;
	pid_t pid;
	uid_t uid;
	int signal;
	int code;
	int status;
	struct task_cputime cputime;
};

void proc_parent_event_release(struct proc_parent_event *event);

size_t proc_unlink_child(struct proc_struct *child,
		struct proc_orphan_event *events,  size_t capacity);

size_t proc_reparent_children(struct proc_struct *proc,
		struct proc_orphan_event *events, size_t capacity);

__must_check
struct proc_struct *proc_parent_get(struct proc_struct *proc);

__must_check
struct task_struct *proc_leader_get(struct proc_struct *proc);

uint32_t proc_child_count(const struct proc_struct *proc);

size_t proc_task_snapshot(struct proc_struct *proc,  const struct task_struct *ignored,
			  struct task_struct **tasks, size_t capacity);

size_t proc_pgrp_task_snapshot(struct pgrp_struct *pgrp, struct session_struct *session,
			       struct task_struct **tasks, size_t capacity);

/** Proc lifecycle and wait-visible event operations. */
bool proc_begin_group_exit(struct proc_struct *proc, int status);

__must_check
bool proc_group_exit_pending(const struct proc_struct *proc, int *status);

void proc_publish_stop(struct proc_struct *proc, int sig, struct proc_parent_event *event);

void proc_publish_continue(struct proc_struct *proc, struct proc_parent_event *event);

void proc_prepare_exit(struct proc_struct *proc, int status, uid_t uid,
		       bool auto_reap, bool notify_sigchld);

size_t proc_publish_exit(struct proc_struct *proc, struct proc_orphan_event *events,
			 size_t capacity, struct proc_parent_event *parent_event);

__must_check
bool proc_can_reap(const struct proc_struct *proc);

size_t proc_mark_reaped(struct proc_struct *proc, struct proc_orphan_event *events, size_t capacity);

/** Serialize exec and de-threading through proc-owned task membership. */
int proc_exec_begin(struct proc_struct *proc, struct task_struct *owner);

int proc_exec_request_siblings(struct proc_struct *proc, struct task_struct *owner);

int proc_exec_wait(struct proc_struct *proc, struct task_wait *wait);

int proc_exec_adopt_pid(struct proc_struct *proc, struct task_struct *owner);

void proc_exec_end(struct proc_struct *proc, struct task_struct *owner);

enum proc_wait_result {
	PROC_WAIT_NO_CHILD,
	PROC_WAIT_NO_EVENT,
	PROC_WAIT_EVENT,
};

enum proc_wait_child_class {
	PROC_WAIT_SIGCHLD,
	PROC_WAIT_CLONE,
	PROC_WAIT_ALL_CHILDREN,
};

struct proc_wait_selector {
	pid_t pid;
	pid_t pgid;
	pid_t creator_tid;
	enum proc_wait_child_class child_class;
	bool creator_only;
};

struct proc_wait_claim {
	struct proc_struct *parent;
	struct proc_struct *child;
	uint64_t generation;
	uint32_t event;
	int status;
	/** Heap snapshot of orphan events, allocated by the syscall layer. */
	struct proc_orphan_event *orphan_events;
	size_t orphan_capacity;
	size_t orphan_count;
};

enum proc_wait_result proc_wait_claim(struct proc_struct *parent,
				      const struct proc_wait_selector *selector,
		                      uint32_t event_mask, struct proc_wait_claim *claim);

int proc_wait_watch(struct proc_struct *parent,
		    const struct proc_wait_selector *selector,
		    uint32_t event_mask, struct task_wait *wait);

bool proc_wait_commit(struct proc_wait_claim *claim);

void proc_wait_abort(struct proc_wait_claim *claim);

/** Process-group/session topology. */
int proc_join_pgrp(struct proc_struct *proc, pid_t pgid, struct proc_orphan_event *event);

/*
 * Create a new session for @p proc.  The caller leaves the whole old
 * session, so every pgrp of the old session is re-evaluated for an orphan
 * transition; events with pgrp/session references are filled up to
 * @p capacity and counted in @p event_count (may be NULL).  Release each
 * event with proc_orphan_event_release() outside any lock.
 */
int proc_create_session(struct proc_struct *proc, pid_t *sid,
			struct proc_orphan_event *events, size_t capacity,
			size_t *event_count);

/**
 * @brief Whether a session has no pgrp with remaining members.
 * @param session Session to inspect.
 * @param exclude Proc that is still leaving (e.g. the exiting last member);
 * it is treated as absent so an empty session is recognized immediately
 * instead of waiting for the reaper to unlink it from its pgrp.
 */
bool proc_session_is_empty(const struct session_struct *session, const struct proc_struct *exclude);

int proc_snapshot_topology(const struct proc_struct *proc, pid_t *pgid, pid_t *sid);

/* Fill the identity recorded when an exited process awaits wait4; false when
 * the proc has no exit snapshot (still live or already reaped). */
bool proc_exit_identity(const struct proc_struct *proc, pid_t *pgid, pid_t *sid);

__must_check
bool proc_pgrp_has_member(pid_t pgid, pid_t sid, const struct task_struct *ignored);

#endif

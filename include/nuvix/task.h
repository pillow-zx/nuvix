#ifndef _NUVIX_TASK_H
#define _NUVIX_TASK_H

/**
 * @file task.h
 * @brief The schedulable execution-context object.
 *
 * `task_struct` is deliberately not a process object.  Process identity,
 * parentage, and process wait state live in `proc_struct`. This object owns
 * execution state, task-local signals, credentials, accounting, and its
 * independently shareable file-table and filesystem-context references.
 *
 * Process code owns lifecycle and bindings under task->lock; the home
 * runqueue owns execution state and placement. Signal admission nests the
 * group signal lock before task->lock. Wait registrations are current-only.
 * Publication holds the execution root until retirement; final puts from
 * atomic contexts defer storage destruction to the independent reaper.
 */

#include <nuvix/atomic.h>
#include <nuvix/cleanup.h>
#include <nuvix/compiler.h>
#include <nuvix/cpu.h>
#include <nuvix/cputime.h>
#include <nuvix/list.h>
#include <nuvix/pid.h>
#include <nuvix/printk.h>
#include <nuvix/refcount.h>
#include <nuvix/task_access.h>
#include <nuvix/wait.h>
#include <nuvix/types.h>
#include <arch/task.h>
#include <uapi/signal.h>

enum task_lifecycle {
	TASK_NEW,
	TASK_LIVE,
	TASK_EXITING,
	TASK_DEAD,
};

enum task_run_state {
	TASK_DORMANT,
	TASK_MIGRATING,
	TASK_RETIRED,
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_BLOCKED,
	TASK_STOPPED,
};

enum task_exit_request {
	TASK_EXIT_REQUEST_NONE,
	TASK_EXIT_REQUEST_EXEC,
};

/** Sleep policy values used only at the wait seam. */
#define TASK_UNINTERRUPTIBLE 0x01u
#define TASK_INTERRUPTIBLE   0x02u
#define TASK_KILLABLE	     0x04u
#define TASK_ANY_SLEEP                                                         \
	(TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE | TASK_KILLABLE)

#define TASK_FLAG_IDLE (1u << 0)

#define TASK_SLEEP_LOCK_MAX 16u

enum task_sleep_lock_kind {
	TASK_SLEEP_LOCK_MUTEX,
	TASK_SLEEP_LOCK_RW_READ,
	TASK_SLEEP_LOCK_RW_WRITE,
};

struct task_sleep_lock_entry {
	const void *lock;
	uint16_t rank;
	enum task_sleep_lock_kind kind;
};

#define KSTACK_ORDER ARCH_KSTACK_ORDER
#define KSTACK_SIZE  ARCH_KSTACK_SIZE

struct proc_struct;
struct vfork_completion;
struct files_struct;
struct fs_struct;
struct mm_struct;
struct sighand_struct;
struct signal_struct;
struct signal_frame_state;
struct trap_frame;

/*
 * Maximum number of supplementary groups. The Linux ABI allows NGROUPS_MAX
 * (65536); nuvix caps the per-task list at 32 entries.
 */
#define NGROUPS_MAX 32

/**
 * @struct cred
 * @brief Immutable task-local credentials with explicit lifetime.
 */
struct cred {
	refcount_t refs;
	uid_t ruid;
	uid_t euid;
	uid_t suid;
	uid_t fsuid;
	gid_t rgid;
	gid_t egid;
	gid_t sgid;
	gid_t fsgid;
	uint32_t ngroups;
	gid_t groups[NGROUPS_MAX];
};

struct task_signal_context {
	uint64_t blocked;
	/* Task-directed signal state (blocked/pending/forced_pending/
	 * pending_info) is guarded by the owning thread group's
	 * signal.siglock.  Consumers that read the fact bits lock-free must
	 * treat them as hints validated under siglock. */
	uint64_t pending;
	uint64_t forced_pending;
	siginfo_t pending_info[NSIG + 1];
	/* Fact bits maintained by one centralized recalc under the thread-group
	 * siglock and read atomically by wait predicates.  The interruptible bit
	 * means a pending signal is currently deliverable to this Task; blocked
	 * pending signals do not interrupt ordinary waits. */
	atomic_t has_pending_signal;
	atomic_t has_fatal_pending;
	struct signal_frame_state *signal_frames;
	uint64_t restore_mask;
	bool restore_mask_pending;
	struct stack_t sas;
	int *set_child_tid;
};

struct restart_context {
	uintptr_t pc;
	uintptr_t args[6];
	uintptr_t nr;
	bool valid;
	bool restartable;
};

struct task_sched_entity {
	struct list_head run_node;
	atomic_t need_resched;
	/* Read from sched_notify_reaper() without the home rq lock. */
	atomic64_t park_generation;
	bool park_active;
	bool notified;
	bool event_fired;
	enum task_wait_policy park_policy;
	uint64_t park_signal_set;
	uint64_t runtime_start;
	uint64_t slice_left;
	struct list_head affinity_waiters;
	uint64_t affinity_sequence;
};

/**
 * @struct task_struct
 * @brief One independently schedulable execution context.
 *
 * The architecture state remains first because `entry.S` consumes fixed
 * offsets generated for this prefix.  Scheduler-owned fields are present
 * for every schedulable CPU.
 */
struct task_struct {
	struct task_state arch;

	refcount_t refs;
	spinlock_t lock;
	uint64_t identity;
	struct mm_struct *mm;
	struct pid_identity *tid;
	struct proc_struct *proc;
	struct cred *cred;
	/* Each task owns one reference per binding. Only current or an
	 * unpublished constructor may replace them; no remote borrowed reads.
	 * Sharing is selected independently by CLONE_FILES and CLONE_FS.
	 * Resource contents remain protected by their own module locks. */
	struct files_struct *files;
	struct fs_struct *fs;

	enum task_lifecycle lifecycle;
	enum task_exit_request exit_request;
	enum task_run_state run_state;
	uint32_t flags;
	struct cpu *cpu;
	cpumask_t allowed_cpus;

	struct task_sched_entity sched;
	struct task_wait wait;
	IFDEF(CONFIG_DEBUG_CONTEXT,
	      struct task_sleep_lock_entry sleep_locks[TASK_SLEEP_LOCK_MAX];
	      uint32_t sleep_lock_depth;)
	struct task_signal_context signal;
	struct restart_context restart;
	struct task_cputime cputime;

	int exit_code;
	struct list_head proc_node;
	struct list_head retired_node;
	/* Proc reference retained until the reaper publishes process exit. */
	struct vfork_completion *vfork;
	bool published;
};

extern struct task_struct *init_task;

/* Idle tasks are static per-CPU storage: they carry no PID, credentials,
 * reference count, or reaper lifecycle. task_is_idle() is the only generic
 * identity test; it is null-safe. */
__must_check __pure
static inline bool task_is_idle(const struct task_struct *task)
{
	return task && (task->flags & TASK_FLAG_IDLE);
}

#include <arch/task_access.h>

/* Credentials. */
__must_check __malloc
struct cred *cred_alloc_root(void);

__must_check __malloc
struct cred *cred_dup(const struct cred *source);

void cred_get(struct cred *cred);

int task_set_uid(struct task_struct *task, uid_t uid);

int task_set_gid(struct task_struct *task, gid_t gid);

int task_set_groups(struct task_struct *task, const gid_t *groups,
		    uint32_t ngroups);

/* Task construction, publication, and lifetime. */
void task_init(void);

__must_check __malloc
struct task_struct *task_alloc(void);

int task_prepare_kernel(struct task_struct *task);

int task_prepare_user_proc(struct task_struct *task, struct proc_struct *proc);

int task_create_initial_proc(struct task_struct *task);

int task_init_resources(struct task_struct *task);

void task_release_resources(struct task_struct *task);

/* Current-task exit or unpublished teardown only. May sleep; no caller
 * locks. Detaches both bindings before dropping their owned references. */
void task_release_file_context(struct task_struct *task);

/* Replacement consumes mm's reference; returns the withdrawn reference.
 * Only current, an unpublished constructor, or the off-CPU reaper writes. */
struct mm_struct *task_replace_mm(struct task_struct *task, struct mm_struct *mm);
struct mm_struct *task_mm_get(struct task_struct *task);

int task_publish(struct task_struct *task);
void task_reap_deferred(void);

void task_unpublish(struct task_struct *task);

__must_check
bool task_try_get(struct task_struct *task);

__must_check
bool task_try_get_live(struct task_struct *task);

void task_put(struct task_struct *task);

CLEANUP_DEFINE(task_ref, struct task_struct *, if (_T) task_put(_T));

/** Look up a target; @p owned receives a cross-task reference, if any. */
__must_check __nonnull(2)
static inline struct task_struct *task_get_target(pid_t pid,
						   struct task_struct **owned)
{
	*owned = pid ? pid_lookup_task(pid) : NULL;
	return pid ? *owned : current_task();
}

void task_free(struct task_struct *task);

/*
 * Task-local sleeping-lock semantic tracker.  Ownership and rank state
 * follow the Task across descheduling and CPU migration; the internal
 * state spinlocks of sleeping locks are never passed here.  Kept inline so
 * non-debug builds pay no external call on every lock fast path.
 */
#ifdef CONFIG_DEBUG_CONTEXT
static inline void task_sleep_lock_acquire(const void *lock, uint16_t rank,
					   enum task_sleep_lock_kind kind)
{
	struct task_struct *task = current_task();
	uint32_t depth;

	BUG_ON(!task || !lock || rank == 0);
	depth = task->sleep_lock_depth;
	BUG_ON(depth >= TASK_SLEEP_LOCK_MAX);
	for (uint32_t index = 0; index < depth; index++)
		if (task->sleep_locks[index].lock == lock)
			panic("recursive sleeping lock: lock=%p rank=%u kind=%u",
			      lock, rank, kind);
	if (depth && rank <= task->sleep_locks[depth - 1].rank)
		panic("sleeping lock rank not increasing: lock=%p rank=%u "
		      "top=%p rank=%u", lock, rank,
		      task->sleep_locks[depth - 1].lock,
		      task->sleep_locks[depth - 1].rank);
	task->sleep_locks[depth] = (struct task_sleep_lock_entry){
		.lock = lock,
		.rank = rank,
		.kind = kind,
	};
	task->sleep_lock_depth = depth + 1;
}

static inline void task_sleep_lock_release(const void *lock,
					   enum task_sleep_lock_kind kind)
{
	struct task_struct *task = current_task();
	uint32_t depth;

	BUG_ON(!task || !lock);
	depth = task->sleep_lock_depth;
	if (depth == 0 || task->sleep_locks[depth - 1].lock != lock ||
	    task->sleep_locks[depth - 1].kind != kind)
		panic("sleeping lock LIFO: release=%p kind=%u top=%p kind=%u",
		      lock, kind,
		      depth ? task->sleep_locks[depth - 1].lock : NULL,
		      depth ? task->sleep_locks[depth - 1].kind : 0);
	task->sleep_locks[depth - 1] = (struct task_sleep_lock_entry){};
	task->sleep_lock_depth = depth - 1;
}
#else
static inline void task_sleep_lock_acquire(const void *lock, uint16_t rank,
					   enum task_sleep_lock_kind kind)
{
	(void)lock;
	(void)rank;
	(void)kind;
}

static inline void task_sleep_lock_release(const void *lock,
					   enum task_sleep_lock_kind kind)
{
	(void)lock;
	(void)kind;
}
#endif

__must_check
bool task_begin_exit(struct task_struct *task);

bool task_request_exec_exit(struct task_struct *task);

bool task_request_group_exit(struct task_struct *task);

__must_check
bool task_exec_exit_requested(struct task_struct *task);

void task_mark_dead(struct task_struct *task);

/** Called only after sched_retired_pop() supplied the Retirement witness. */
__must_check
bool task_reap_ready(const struct task_struct *task);

/** Detach the task PID role; only the independent reaper may call this. */
void task_reap_unpublish(struct task_struct *task);

/* Scheduler and architecture entry points. */
void arch_task_init(struct task_struct *task);

void task_setup_kthread(struct task_struct *task, void (*fn)(void *), void *arg);

void task_setup_clone_frame(struct task_struct *child, const struct trap_frame *parent_tf,
			    unsigned long flags, uintptr_t child_stack, uintptr_t tls);

__must_check
bool task_trap_frome_user(const struct task_struct *task);

__must_check __malloc __nonnull(1)
struct task_struct *kernel_thread(void (*fn)(void *), void *arg);

void set_init_task(struct task_struct *task);

/* CPU accounting and task-local state snapshots. */
static inline uint64_t task_user_ticks(const struct task_struct *task)
{
	return task ? task->cputime.utime_ticks : 0;
}

static inline uint64_t task_system_ticks(const struct task_struct *task)
{
	return task ? task->cputime.stime_ticks : 0;
}

static inline void task_set_exit_code(struct task_struct *task, int code)
{
	if (task)
		task->exit_code = code;
}

static inline bool task_is_exiting(const struct task_struct *task)
{
	return task && task->lifecycle >= TASK_EXITING;
}

static inline bool task_is_live(const struct task_struct *task)
{
	return task && task->lifecycle == TASK_LIVE;
}

static inline bool task_is_blocked(const struct task_struct *task)
{
	return task && task->run_state == TASK_BLOCKED;
}

static inline bool task_is_queued(const struct task_struct *task)
{
	return task && task->run_state == TASK_RUNNABLE;
}

static inline uint8_t task_need_resched(const struct task_struct *task)
{
	return task ? (uint8_t)atomic_read_acquire(&task->sched.need_resched)
		    : 0;
}

static inline void task_set_need_resched(struct task_struct *task,
					 uint8_t value)
{
	if (task)
		atomic_set_release(&task->sched.need_resched, value);
}

static inline uid_t task_uid(const struct task_struct *task)
{
	return task && task->cred ? task->cred->ruid : 0;
}

static inline uid_t task_euid(const struct task_struct *task)
{
	return task && task->cred ? task->cred->euid : 0;
}

static inline gid_t task_gid(const struct task_struct *task)
{
	return task && task->cred ? task->cred->rgid : 0;
}

static inline gid_t task_egid(const struct task_struct *task)
{
	return task && task->cred ? task->cred->egid : 0;
}

/*
 * Membership check used by the VFS group permission check. The primary
 * comparison uses egid (Linux checks the group bits against fsgid; nuvix
 * never sets fsgid independently, so egid is the correct anchor today and
 * after a future setfsgid).
 */
__must_check __pure
static inline bool task_in_group(const struct task_struct *task, gid_t gid)
{
	const struct cred *cred;

	if (!task)
		return false;
	cred = task->cred;
	if (!cred)
		return false;
	if (cred->egid == gid)
		return true;
	for (uint32_t i = 0; i < cred->ngroups; i++)
		if (cred->groups[i] == gid)
			return true;
	return false;
}

#endif

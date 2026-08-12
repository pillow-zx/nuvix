#ifndef _CUTEOS_KERNEL_TASK_H
#define _CUTEOS_KERNEL_TASK_H

/**
 * @file task.h
 * @brief The schedulable execution-context object.
 *
 * `task_struct` is deliberately not a process object.  Process identity,
 * shared resources, parentage, and process wait state live in `proc_struct`;
 * this object owns only task lifecycle, run state, architecture context,
 * task-local signal state, credentials, and accounting.
 *
 * Locking and lifetime: lifecycle transitions run under the task's own
 * wait lock (LOCK_RANK_WAIT, 40); run state, on_rq, and CPU assignment are
 * scheduler-owned.  task_get()/task_put() are plain refcount operations, but
 * a final put destroys storage (kernel stack, TID role): never call
 * task_put() while holding any lock.  task_try_get_live() pairs a refcount
 * acquire with a wait-lock lifecycle read (pid lock 20 before wait lock 40).
 * task_reap_unpublish() is reserved for the independent reaper.
 */

#include <kernel/atomic.h>
#include <kernel/compiler.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/pid.h>
#include <kernel/proc.h>
#include <kernel/refcount.h>
#include <kernel/rseq_types.h>
#include <kernel/wait.h>
#include <kernel/types.h>
#include <arch/task.h>
#include <uapi/signal.h>

enum task_lifecycle {
	TASK_NEW,
	TASK_LIVE,
	TASK_EXITING,
	TASK_DEAD,
};

enum task_run_state {
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

#define KSTACK_ORDER ARCH_KSTACK_ORDER
#define KSTACK_SIZE  ARCH_KSTACK_SIZE

struct files_struct;
struct fs_struct;
struct mm_struct;
struct sighand_struct;
struct signal_struct;
struct robust_list_head;
struct signal_frame_state;
struct trap_frame;

/*
 * Maximum number of supplementary groups. The Linux ABI allows NGROUPS_MAX
 * (65536); cuteOS caps the per-task list at the musl user-space NGROUPS_MAX.
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
	uint64_t pending;
	uint64_t forced_pending;
	siginfo_t pending_info[NSIG + 1];
	struct signal_frame_state *signal_frames;
	uint64_t restore_mask;
	bool restore_mask_pending;
	struct stack_t sas;
	int *set_child_tid;
	int *clear_child_tid;
	struct robust_list_head *robust_list;
	size_t robust_list_len;
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
};

/**
 * @struct task_struct
 * @brief One independently schedulable execution context.
 *
 * The architecture state remains first because `entry.S` consumes fixed
 * offsets generated for this prefix.  Scheduler-owned fields are present
 * from the first build even though only CPU 0 is online today.
 */
struct task_struct {
	struct task_state arch;

	refcount_t refs;
	struct pid_identity *tid;
	struct proc_struct *proc;
	struct cred *cred;

	enum task_lifecycle lifecycle;
	enum task_exit_request exit_request;
	enum task_run_state run_state;
	uint32_t flags;
#define TASK_FLAG_IDLE (1u << 0)
	struct cpu *cpu;
	bool on_rq;
	bool on_cpu;
	uint64_t allowed_cpus;

	struct task_sched_entity sched;
	struct task_wait wait;
	struct task_signal_context signal;
	struct restart_context restart;
	struct rseq_task_context rseq;
	struct task_cputime cputime;

	int exit_code;
	struct list_head proc_node;
	struct list_head retired_node;
	/* Proc reference retained until the reaper publishes process exit. */
	struct proc_struct *reap_proc;
	bool published;
};

extern struct task_struct *init_task;

/* Idle tasks are static per-CPU storage: they carry no PID, credentials,
 * reference count, or reaper lifecycle. task_is_idle() is the only generic
 * identity test; it is null-safe. */
__always_inline __must_check __pure
static inline bool task_is_idle(const struct task_struct *task)
{
	return task && (task->flags & TASK_FLAG_IDLE);
}

#include <arch/task_access.h>

/* Credentials. */
__must_check
struct cred *cred_alloc_root(void);

__must_check
struct cred *cred_dup(const struct cred *source);

void cred_get(struct cred *cred);

void cred_put(struct cred *cred);

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

void task_publish(struct task_struct *task);

void task_unpublish(struct task_struct *task);

__must_check
bool task_try_get(struct task_struct *task);

__must_check
bool task_try_get_live(struct task_struct *task);

void task_put(struct task_struct *task);

void task_free(struct task_struct *task);

__must_check
bool task_begin_exit(struct task_struct *task);

__must_check
bool task_request_exec_exit(struct task_struct *task);

bool task_request_group_exit(struct task_struct *task);

__must_check
bool task_exec_exit_requested(struct task_struct *task);

void task_mark_dead(struct task_struct *task);

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
	return task && task->on_rq;
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
 * comparison uses egid (Linux checks the group bits against fsgid; cuteOS
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

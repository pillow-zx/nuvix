#ifndef _CUTEOS_KERNEL_TASK_H
#define _CUTEOS_KERNEL_TASK_H

/**
 * @file task.h
 * @brief 进程控制块、线程组关系与 task 生命周期公共接口。
 */

#include <kernel/types.h>
#include <kernel/atomic.h>
#include <kernel/list.h>
#include <kernel/refcount.h>
#include <kernel/wait.h>
#include <kernel/compiler.h>
#include <kernel/cpu.h>
#include <kernel/rseq_types.h>
#include <arch/task.h>
#include <uapi/signal.h>

/**
 * @def TASK_RUNNING
 * @brief task is runnable or currently executing on the single CPU.
 */
constexpr uint32_t TASK_RUNNING = 0x00u;

/**
 * @def TASK_UNINTERRUPTIBLE
 * @brief task sleeps until an explicit wakeup, ignoring pending signals.
 */
constexpr uint32_t TASK_UNINTERRUPTIBLE = 0x01u;

/**
 * @def TASK_INTERRUPTIBLE
 * @brief task sleeps until wakeup or signal delivery makes it runnable.
 */
constexpr uint32_t TASK_INTERRUPTIBLE = 0x02u;

/**
 * @def TASK_KILLABLE
 * @brief task sleeps until wakeup or pending SIGKILL makes it runnable.
 */
constexpr uint32_t TASK_KILLABLE = 0x20u;

/**
 * @def TASK_ZOMBIE
 * @brief task has exited and keeps waitable exit status for its parent.
 */
constexpr uint32_t TASK_ZOMBIE = 0x04u;

/**
 * @def TASK_DEAD
 * @brief task resources have been released and the task is no longer runnable.
 */
constexpr uint32_t TASK_DEAD = 0x08u;

/**
 * @def TASK_STOPPED
 * @brief task is stopped by job-control style state, not eligible to run.
 */
constexpr uint32_t TASK_STOPPED = 0x10u;

/**
 * @def TASK_ANY_SLEEP
 * @brief Mask matching all wait sleep states.
 */
constexpr uint32_t TASK_ANY_SLEEP =
	TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE | TASK_KILLABLE;

/**
 * @def KSTACK_ORDER
 * @brief Architecture-selected allocation order for every task kernel stack.
 */
constexpr uint32_t KSTACK_ORDER = ARCH_KSTACK_ORDER;

/**
 * @def KSTACK_SIZE
 * @brief Size in bytes of the per-task kernel stack.
 */
constexpr size_t KSTACK_SIZE = ARCH_KSTACK_SIZE;

struct files_struct;
struct fs_struct;
struct mm_struct;
struct sighand_struct;
struct signal_struct;
struct robust_list_head;
struct task_struct;
struct signal_frame_state;

/**
 * @struct task_identity
 * @brief Linux-visible task identifiers and thread-group leadership.
 *
 * @par Fields
 * - @c pid: Kernel thread id; userspace observes it through gettid.
 * - @c tgid: Thread-group id; userspace observes it through getpid.
 * - @c pgid: Process-group id used by setpgid/getpgid semantics.
 * - @c sid: Session id used by setsid/getsid and controlling tty policy.
 * - @c group_leader: Leader whose pid equals @ref tgid.
 */
struct task_identity {
	pid_t pid;
	pid_t tgid;
	pid_t pgid;
	pid_t sid;
	struct task_struct *group_leader;
};

/**
 * @struct task_process_identity
 * @brief A process-identity snapshot protected by task/process ownership.
 *
 * @par Fields
 * - @c pgid: Process group sampled with @c sid.
 * - @c sid: Session sampled with @c pgid.
 *
 * Callers must use task_process_snapshot() rather than independently reading
 * SID and PGID when the pair controls another subsystem's policy.
 */
struct task_process_identity {
	pid_t pgid;
	pid_t sid;
};

/**
 * @enum task_child_event_type
 * @brief Lifecycle edge reported by one child to its wait parent.
 */
enum task_child_event_type {
	TASK_CHILD_EVENT_NONE,
	TASK_CHILD_EVENT_EXIT,
	TASK_CHILD_EVENT_STOP,
	TASK_CHILD_EVENT_CONTINUE,
};

constexpr uint32_t TASK_CHILD_EVENT_MASK_EXIT = 1u << TASK_CHILD_EVENT_EXIT;
constexpr uint32_t TASK_CHILD_EVENT_MASK_STOP = 1u << TASK_CHILD_EVENT_STOP;
constexpr uint32_t TASK_CHILD_EVENT_MASK_CONTINUE =
	1u << TASK_CHILD_EVENT_CONTINUE;

/**
 * @enum task_child_wait_state
 * @brief Result of observing a parent's matching child-event stream.
 */
enum task_child_wait_state {
	TASK_CHILD_WAIT_NO_CHILD,
	TASK_CHILD_WAIT_NO_EVENT,
	TASK_CHILD_WAIT_EVENT,
};

/**
 * @struct task_child_event_claim
 * @brief One ordered child event held until wait4 copies its result.
 *
 * The task module owns the queued record.  Callers may inspect this snapshot
 * but must finish it with task_child_event_commit() or
 * task_child_event_abort().
 */
struct task_child_event_claim {
	struct task_struct *parent;
	struct task_struct *child;
	pid_t pid;
	uint64_t sequence;
	enum task_child_event_type type;
	int status;
};

/**
 * @struct task_lifecycle
 * @brief Run state and exit status owned by scheduler and wait paths.
 *
 * @par Fields
 * - @c state: TASK_* state sampled by wake/schedule paths.
 * - @c exit_code: Linux-encoded wait status recorded at exit.
 * - @c exit_signal: Signal delivered to parent when this task exits.
 * - @c child_events: Ordered, task-owned wait-visible child lifecycle edges.
 * - @c next_child_event_sequence: Sequence assigned to the next child edge.
 * - @c user_process: One-way atomic role set after a successful user exec and
 *   inherited by clone; it survives user-mm teardown during exit.
 */
struct task_lifecycle {
	refcount_t refs;
	volatile uint32_t state;
	int exit_code;
	int exit_signal;
	struct list_head child_events;
	uint64_t next_child_event_sequence;
	bool published;
	atomic_t user_process;
};

/**
 * @struct task_vfork_context
 * @brief Clone-owned completion state for a vfork child.
 *
 * @par Fields
 * - @c lock: Protects completed as the wait source lock.
 * - @c completion_wait: Calling task waits for a possible completion change.
 * - @c active: Initialized before publish and immutable afterwards; this task
 *   was published as a vfork child.
 * - @c completed: The child no longer uses its pre-exec address space.
 */
struct task_vfork_context {
	spinlock_t lock;
	struct wait_channel completion_wait;
	bool active;
	bool completed;
};

/**
 * @struct task_links
 * @brief Intrusive links connecting parent/child and thread-group topology.
 *
 * @par Fields
 * - @c parent: Reaper/wait parent, or NULL for root.
 * - @c children: Head of children whose parent is this task.
 * - @c sibling: Node in parent's children list.
 * - @c thread_group: Head of threads when this task is leader.
 * - @c thread_node: Node in group leader's thread list.
 * - @c wait_child_queue: wait4 sleepers for children.
 *
 * Parent/child links, child event records, and the child wait queue are owned
 * by the task module. Cross-subsystem callers use task_child_*() operations
 * instead of reading or locking these fields directly.
 */
struct task_links {
	struct task_struct *parent;
	struct list_head children;
	struct list_head sibling;
	struct list_head thread_group;
	struct list_head thread_node;
	struct wait_channel wait_child_queue;
};

/**
 * @struct task_resources
 * @brief Shared subsystem resources referenced by a task.
 *
 * @par Fields
 * - @c mm: User address space; NULL for pure kernel tasks.
 * - @c files: File descriptor table.
 * - @c fs: cwd/root/umask state.
 * - @c sighand: Installed signal actions.
 * - @c signal: Thread-group shared signal state.
 * - @c uid: Current real/effective uid in the simplified cred model.
 * - @c gid: Current real/effective gid in the simplified cred model.
 */
struct task_resources {
	struct mm_struct *mm;
	struct files_struct *files;
	struct fs_struct *fs;
	struct sighand_struct *sighand;
	struct signal_struct *signal;
	uid_t uid;
	gid_t gid;
};

/**
 * @struct task_signal_context
 * @brief Per-thread signal, futex robust-list, and alt-stack state.
 *
 * @par Fields
 * - @c blocked: Linux signal mask; bit n represents signal n+1.
 * - @c pending: Per-thread pending signal mask.
 * - @c forced_pending: Pending signals that bypass PID 1 default protection.
 * - @c signal_frames: Signal-owned LIFO state for active user frames.
 * - @c restore_mask: Signal mask restored after a temporary wait mask.
 * - @c restore_mask_pending: Whether restore_mask must be consumed.
 * - @c sas: sigaltstack state copied to/from userspace ABI.
 * - @c clear_child_tid: User futex word cleared by set_tid_addr exit.
 * - @c robust_list: User robust futex list head.
 * - @c robust_list_len: Userspace-reported robust-list head size.
 */
struct task_signal_context {
	uint64_t blocked;
	uint64_t pending;
	uint64_t forced_pending;
	siginfo_t pending_info[NSIG];
	struct signal_frame_state *signal_frames;
	uint64_t restore_mask;
	bool restore_mask_pending;
	struct stack_t sas;
	int *clear_child_tid;
	struct robust_list_head *robust_list;
	size_t robust_list_len;
};

/**
 * @struct restart_context
 * @brief Dispatcher-owned user context for an interrupted syscall restart.
 *
 * A valid context is retained only while a restartable syscall has returned
 * -EINTR and is awaiting signal delivery.  The signal module consumes it
 * through the syscall restart interface; no syscall handler rewrites the
 * trap frame itself.
 */
struct restart_context {
	uintptr_t pc;
	uintptr_t args[6];
	uintptr_t nr;
	bool valid;
	bool restartable;
};

/**
 * @struct task_sched_entity
 * @brief Scheduler-private runnable metadata embedded in a task.
 *
 * @par Fields
 * - @c run_list: Node in the selected run queue.
 * - @c need_resched: Deferred reschedule request consumed at a safe
 *   scheduler handoff.
 * - @c sched_level: Current MLFQ priority level.
 * - @c time_slice: Remaining ticks in the current queue level.
 * - @c sched_ticks: Ticks consumed in the current accounting window.
 * - @c enqueue_jiffies: Time when the task entered a run queue.
 */
struct task_sched_entity {
	struct list_head run_list;
	volatile uint8_t need_resched;
	uint8_t sched_level;
	uint8_t time_slice;
	uint8_t sched_ticks;
	uint64_t enqueue_jiffies;
};

/**
 * @struct task_cputime
 * @brief Tick counters reported through times/getrusage-style interfaces.
 *
 * @par Fields
 * - @c utime_ticks: Timer ticks charged while executing user code.
 * - @c stime_ticks: Timer ticks charged while executing kernel code.
 */
struct task_cputime {
	uint64_t utime_ticks;
	uint64_t stime_ticks;
};

/**
 * @struct task_struct
 * @brief Task lifecycle aggregate and subsystem ownership root.
 *
 * Field groups mirror subsystem ownership. Task owns lifecycle assembly and
 * cross-subsystem identity/resource wiring. Complex per-task semantics belong
 * to the owning subsystem: signal state through signal.h, robust futex and
 * clear_child_tid state through futex.h, rseq state through rseq.h, scheduling
 * policy through sched/, and architecture state through arch task accessors.
 *
 * Helpers in this header are limited to lifecycle aggregation, simple identity
 * and resource wiring, and hot cross-subsystem accessors that do not expose a
 * single subsystem's policy surface.
 *
 * @par Fields
 * - @c arch: RISC-V context, trap frame, stack, satp.
 * - @c ids: PID/TGID/PGID/SID and leader identity.
 * - @c lifecycle: Runnable, sleep, stopped, exit state.
 * - @c vfork: Clone-owned vfork completion state.
 * - @c links: Parent/child and thread-group intrusive links.
 * - @c resources: MM, fd, fs, signal, and credential refs.
 * - @c sigctx: Per-thread signal/futex ABI state.
 * - @c restart: Dispatcher-owned interrupted-syscall context.
 * - @c rseq: Restartable sequences registration.
 * - @c sched: Scheduler queueing and tick state.
 * - @c cputime: CPU time charged to this task.
 * - @c child_cputime: Reaped child CPU time totals.
 * - @c wait_lock: Serializes publication and removal of active wait sessions.
 * - @c active_wait: Opaque wait session cancelled during exit.
 */
struct task_struct {
	struct task_state arch;
	struct task_identity ids;
	struct task_lifecycle lifecycle;
	struct task_vfork_context vfork;
	struct task_links links;
	struct task_resources resources;
	struct task_signal_context sigctx;
	struct restart_context restart;
	struct rseq_task_context rseq;
	struct task_sched_entity sched;
	struct task_cputime cputime;
	struct task_cputime child_cputime;
	spinlock_t wait_lock;
	struct wait_session *active_wait;
};

extern struct task_struct idle_task;

extern struct task_struct *init_task;

#include <arch/task_access.h>

/**
 * @brief Return a task's user address space.
 * @param task Task to inspect, or NULL.
 * @return The task mm, or NULL for NULL/kernel-only tasks.
 */
__must_check __pure
static inline struct mm_struct *task_mm(const struct task_struct *task)
{
	return task ? task->resources.mm : NULL;
}

/**
 * @brief Replace a task's user address-space pointer.
 * @param task Task to update, or NULL.
 * @param mm New mm pointer; may be NULL for kernel tasks or after teardown.
 */
static inline void task_set_mm(struct task_struct *task, struct mm_struct *mm)
{
	if (task)
		task->resources.mm = mm;
}

__must_check __pure __nonnull(1)
static inline struct files_struct *task_files(struct task_struct *task)
{
	return task->resources.files;
}

__must_check __pure
static inline struct files_struct *task_files_safe(struct task_struct *task)
{
	return task ? task_files(task) : NULL;
}

static inline void task_set_files(struct task_struct *task,
				  struct files_struct *files)
{
	if (task)
		task->resources.files = files;
}

__must_check __pure
static inline struct fs_struct *task_fs(struct task_struct *task)
{
	return task ? task->resources.fs : NULL;
}

static inline void task_set_fs(struct task_struct *task, struct fs_struct *fs)
{
	if (task)
		task->resources.fs = fs;
}

__must_check __pure __nonnull(1)
static inline uid_t task_uid(const struct task_struct *task)
{
	return task->resources.uid;
}

__must_check __pure __nonnull(1)
static inline gid_t task_gid(const struct task_struct *task)
{
	return task->resources.gid;
}

__must_check __pure __nonnull(1)
static inline pid_t task_pid(const struct task_struct *task)
{
	return task->ids.pid;
}

/**
 * @brief Return the Linux thread-group id observed by getpid().
 * @param task Non-NULL task.
 * @return Thread-group id; equal to the group leader pid.
 */
__must_check __pure __nonnull(1)
static inline  pid_t task_tgid(const struct task_struct *task)
{
	return task->ids.tgid;
}

static inline void task_set_uid(struct task_struct *task, uid_t uid)
{
	BUG_ON(!task);
	task->resources.uid = uid;
}

static inline void task_set_gid(struct task_struct *task, gid_t gid)
{
	BUG_ON(!task);
	task->resources.gid = gid;
}

__must_check __pure __nonnull(1)
static inline uint32_t task_state(const struct task_struct *task)
{
	return task->lifecycle.state;
}

__must_check __pure
static inline uint32_t task_state_safe(const struct task_struct *task)
{
	return task ? task_state(task) : TASK_DEAD;
}

static inline void task_set_state(struct task_struct *task, uint32_t state)
{
	if (task)
		task->lifecycle.state = state;
}

static inline void task_mark_running(struct task_struct *task)
{
	task_set_state(task, TASK_RUNNING);
}

static inline void task_mark_interruptible_sleep(struct task_struct *task)
{
	task_set_state(task, TASK_INTERRUPTIBLE);
}

static inline void task_mark_killable_sleep(struct task_struct *task)
{
	task_set_state(task, TASK_KILLABLE);
}

static inline void task_mark_uninterruptible_sleep(struct task_struct *task)
{
	task_set_state(task, TASK_UNINTERRUPTIBLE);
}

static inline void task_mark_stopped(struct task_struct *task)
{
	task_set_state(task, TASK_STOPPED);
}

__must_check __pure __nonnull(1)
static inline struct task_struct *task_group_leader(struct task_struct *task)
{
	return task->ids.group_leader;
}

__must_check __pure
static inline struct task_struct *task_group_leader_safe(struct task_struct *task)
{
	return task ? task_group_leader(task) : NULL;
}

/**
 * @brief Return a task's reaper parent, or NULL when it has none.
 *
 * The returned pointer is not lifecycle-pinned; callers must use it only for
 * the immediate current-task relationship checks supported by task lifecycle.
 */
struct task_struct *task_parent(struct task_struct *task);

/**
 * @brief Link one non-thread child to its wait parent.
 */
void task_link_child(struct task_struct *parent, struct task_struct *child);

/**
 * @brief Remove a child from its wait parent.
 */
void task_unlink_child(struct task_struct *task);

/**
 * @brief Count direct children of one parent.
 */
__must_check
uint32_t task_child_count(const struct task_struct *parent);

/**
 * @brief Publish a group leader's stop edge and notify its wait parent.
 */
void task_child_publish_stop(struct task_struct *task, int sig);

/**
 * @brief Publish a group leader's continuation edge and notify its parent.
 */
void task_child_publish_continue(struct task_struct *task);

/**
 * @brief Reparent children, publish a group leader's exit, and notify parent.
 * @param task Exiting group leader.
 * @param status Linux-encoded wait status.
 */
void task_child_publish_exit(struct task_struct *task, int status);

/**
 * @brief Claim the first matching queued event for one parent and pid filter.
 * @param parent Parent whose children are inspected.
 * @param pid -1 for any child, otherwise one positive child PID.
 * @param event_mask Bitset of TASK_CHILD_EVENT_MASK_* values.
 * @param claim Output held event when TASK_CHILD_WAIT_EVENT is returned.
 */
__must_check __nonnull(1, 4)
enum task_child_wait_state task_child_event_claim_next( struct task_struct *parent,
                pid_t pid, uint32_t event_mask, struct task_child_event_claim *claim);

/**
 * @brief Atomically observe matching child events and register a wait channel.
 *
 * Returns 1 when a child disappeared or a matching event is available, 0 when
 * the session was registered with no matching event, or a negative errno.
 */
__must_check __nonnull(1, 2)
int task_child_event_watch(struct wait_session *session, struct task_struct *parent,
		pid_t pid, uint32_t event_mask);

/**
 * @brief Commit a held child event after userspace result copies succeed.
 * @return true when the exact claimed sequence was consumed.
 */
__must_check __nonnull(1)
bool task_child_event_commit(const struct task_child_event_claim *claim);

/**
 * @brief Return a held event to the queue after a failed userspace copy.
 */
__nonnull(1)
void task_child_event_abort(const struct task_child_event_claim *claim);

/**
 * @brief Detach and mark a reaped zombie dead before PID unpublication.
 */
__nonnull(1)
void task_child_release_zombie(struct task_struct *task) ;

__must_check __pure
static inline struct list_head *
task_thread_group(struct task_struct *task)
{
	return task ? &task->links.thread_group : NULL;
}

__must_check __pure
static inline struct list_head *
task_thread_node(struct task_struct *task)
{
	return task ? &task->links.thread_node : NULL;
}

static inline void task_link_thread(struct task_struct *leader,
				    struct task_struct *thread)
{
	if (!leader || !thread)
		return;
	list_add_tail(&thread->links.thread_node, &leader->links.thread_group);
}

static inline void task_unlink_thread(struct task_struct *task)
{
	if (!task || list_empty(&task->links.thread_node))
		return;
	list_del_init(&task->links.thread_node);
}

__must_check __pure
static inline uint64_t
task_user_ticks(const struct task_struct *task)
{
	return task ? task->cputime.utime_ticks : 0;
}

__must_check __pure
static inline uint64_t
task_system_ticks(const struct task_struct *task)
{
	return task ? task->cputime.stime_ticks : 0;
}

__must_check __pure
static inline int task_exit_code(struct task_struct *task)
{
	return task ? task->lifecycle.exit_code : 0;
}

static inline void task_set_exit_code(struct task_struct *task, int code)
{
	if (task)
		task->lifecycle.exit_code = code;
}

__must_check __pure
static inline int task_exit_signal(struct task_struct *task)
{
	return task ? task->lifecycle.exit_signal : 0;
}

static inline void task_set_exit_signal(struct task_struct *task, int sig)
{
	if (task)
		task->lifecycle.exit_signal = sig;
}

__must_check __pure
static inline struct list_head *
task_run_list(struct task_struct *task)
{
	return task ? &task->sched.run_list : NULL;
}

__must_check __pure
static inline bool task_is_queued(struct task_struct *task)
{
	return task && !list_empty(&task->sched.run_list);
}

__must_check __pure
static inline uint8_t
task_need_resched(struct task_struct *task)
{
	return task ? task->sched.need_resched : 0;
}

static inline void task_set_need_resched(struct task_struct *task, uint8_t val)
{
	if (task)
		task->sched.need_resched = val;
}

/**
 * @brief Initialize global task-management state.
 */
void task_init(void);

/**
 * @brief Allocate an unpublished task with architecture stack storage.
 *
 * The returned task owns one lifecycle reference and a reserved PID, but is
 * invisible to PID lookup until task_publish() commits it.
 * @return New task on success, or NULL when allocation fails.
 */
__must_check __malloc
struct task_struct *task_alloc(void);

/**
 * @brief Initialize reference-counted resources for a new task.
 * @param task Task returned by task_alloc().
 * @return 0 on success, or a negative errno.
 */
__must_check
int task_init_resources(struct task_struct *task);

/**
 * @brief Drop all resources held by a task.
 * @param task Task whose resources are no longer externally reachable.
 */
void task_release_resources(struct task_struct *task);

/**
 * @brief Free a task_struct and its architecture-owned storage.
 * @param task Task to free; may be NULL.
 */
void task_free(struct task_struct *task);

/**
 * @brief Make a fully initialized task discoverable by PID/TID lookup.
 * @param task Unpublished task holding its creator's base reference.
 *
 * Publish only after task resources and parent/thread-group links are ready.
 * A successful PID lookup returns an additional lifecycle reference; it never
 * returns an unpinned raw task pointer.
 */
__nonnull(1) __access_no_size(read_write, 1)
void task_publish(struct task_struct *task);

/**
 * @brief Remove a task from PID/TID lookup before dropping its base reference.
 * @param task Published task that will no longer accept new PID lookups.
 *
 * Existing lookup references remain valid and must be released with
 * task_put().  This is normally the last step of zombie reaping.
 */
__nonnull(1) __access_no_size(read_write, 1)
void task_unpublish(struct task_struct *task);

/**
 * @brief Try to acquire one lifecycle reference without requiring publication.
 * @param task Task to pin, or NULL.
 * @return true when the task remains alive and the caller owns a reference.
 *
 * The statically allocated idle task is always considered alive and does not
 * acquire a reference.  A successful call for every other task must be paired
 * with task_put().
 */
__must_check
bool task_try_get(struct task_struct *task);

/**
 * @brief Drop one task lifecycle reference.
 * @param task Reference returned by lookup, or the creator's base reference.
 */
void task_put(struct task_struct *task);

/**
 * @brief Initialize architecture-owned task fields.
 * @param task Non-NULL task being prepared for execution.
 */
__nonnull(1)
void arch_task_init(struct task_struct *task);

/**
 * @brief Build the initial kernel-thread return frame.
 * @param task Task being initialized.
 * @param fn Kernel function to run.
 * @param arg Opaque argument passed to @p fn.
 */
__nonnull(1, 2)
void task_setup_kthread(struct task_struct *task, void (*fn)(void *), void *arg);

/**
 * @brief Build the child trap frame for clone/fork.
 * @param child Child task being initialized.
 * @param parent_tf Parent trap frame used as the ABI template.
 * @param flags Linux clone flags selected by syscall layer.
 * @param child_stack Optional userspace stack pointer override.
 * @param tls Optional thread-local storage value for clone.
 */
 __nonnull(1, 2)
void task_setup_clone_frame(struct task_struct *child, const struct trap_frame *parent_tf,
				    unsigned long flags, uintptr_t child_stack,
				    uintptr_t tls);

/**
 * @brief Switch active user page-table context between tasks.
 * @param prev Task being switched out.
 * @param next Task being switched in.
 */
 __nonnull(1, 2)
void task_switch_address_space(const struct task_struct *prev,
				       const struct task_struct *next);

/**
 * @brief Switch callee-saved CPU context from @p prev to @p next.
 * @param prev Current task.
 * @param next Next scheduled task.
 */
__nonnull(1, 2)
void task_switch(struct task_struct *prev, struct task_struct *next);

/**
 * @brief Check whether the saved trap frame came from user mode.
 * @param task Task whose architecture state is inspected.
 * @return true when the saved trap frame represents user context.
 */
__must_check __pure
bool task_trap_frome_user(const struct task_struct *task);

#ifdef KERNEL_SELFTEST
__pure
bool arch_task_test_kernel_thread_setup(const struct task_struct *task,
		void (*fn)(void *), void *arg);

__must_check __pure
bool arch_task_test_layout_contract(void);

__nonnull(1)
void arch_task_test_setup_user_return(struct task_struct *task, size_t user_pc,
		size_t user_sp, size_t user_sstatus);
#endif

/**
 * @brief Create a runnable kernel task.
 * @param fn Kernel function to execute.
 * @param arg Opaque argument passed to @p fn.
 * @return New task on success, or NULL on allocation/setup failure.
 */
__must_check __malloc __nonnull(1)
struct task_struct *kernel_thread(void (*fn)(void *), void *arg);

/**
 * @brief Publish the process that becomes PID 1 after exec.
 * @param task Task selected as the init task.
 */
__nonnull(1) __access_no_size(read_only, 1)
void set_init_task(struct task_struct *task);

/**
 * @brief Test whether a task is its thread-group leader.
 * @param task Task to inspect.
 * @return true when task->pid equals task->tgid.
 */
__must_check __pure
bool task_is_group_leader(const struct task_struct *task);

/**
 * @brief Check whether a thread group contains tasks besides its leader.
 * @param task Any task in the group.
 * @return true when another task shares the same tgid.
 */
__must_check __pure
bool task_group_has_other_threads(const struct task_struct *task);

/**
 * @brief Find a thread-group leader by TGID.
 * @param tgid Thread-group id to search for.
 * @return Matching leader with a lifecycle reference, or NULL.
 *
 * Call task_put() after the task is no longer needed.
 */
__must_check
struct task_struct *task_find_group_leader(pid_t tgid);

/**
 * @brief Find a task by Linux TID.
 * @param tid Thread id to search for.
 * @return Matching task with a lifecycle reference, or NULL.
 *
 * Call task_put() after the task is no longer needed.
 */
__must_check
struct task_struct *task_find_thread(pid_t tid);

/**
 * @brief Check whether a task belongs to a thread group.
 * @param task Task to inspect.
 * @param tgid Thread-group id.
 * @return true when @p task is in @p tgid.
 */
__must_check __pure
bool task_in_thread_group(const struct task_struct *task,
					      pid_t tgid);

/**
 * @brief Check whether a task belongs to a user process group.
 *
 * This role is independent of the task's current user-mm pointer, so it
 * remains stable while exit releases resources.
 * @param task Task to inspect.
 * @return true when @p task is a user-process group leader.
 */
__must_check __pure
bool task_is_user_process(const struct task_struct *task);

/**
 * @brief Check whether a process group has a live member in one session.
 * @param pgid Process-group id.
 * @param sid Session id.
 * @param ignored Optional task that must not count as a member.
 * @return true if a live task other than @p ignored has both ids.
 */
__must_check
bool task_pgid_has_live_member_except(
	pid_t pgid, pid_t sid, const struct task_struct *ignored);

/**
 * @brief Read one task's process group and session as one protected snapshot.
 * @param task Lifecycle-pinned task to inspect.
 * @param identity Output process identity snapshot.
 * @return 0 on success, or a negative errno.
 */
__must_check
int task_process_snapshot(const struct task_struct *task,
				       struct task_process_identity *identity);

#ifdef KERNEL_SELFTEST
pid_t task_test_pgid(const struct task_struct *task) __must_check;
pid_t task_test_sid(const struct task_struct *task) __must_check;
void task_test_set_process_identity(struct task_struct *task, pid_t pgid,
				    pid_t sid);
void task_test_mark_user_process(struct task_struct *task);
void task_test_inherit_process_role(struct task_struct *child,
				    const struct task_struct *parent);
#endif

#endif

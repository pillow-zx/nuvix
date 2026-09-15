/*
 * kernel/task.c - schedulable task object and task-local lifecycle
 */

#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/fork.h>
#include <nuvix/fs_struct.h>
#include <nuvix/mm.h>
#include <nuvix/pid.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>

struct task_struct idle_tasks[NR_CPUS];
uint8_t idle_stacks[NR_CPUS][KSTACK_SIZE] __aligned(PAGE_SIZE);
struct task_struct *init_task;
static atomic64_t next_task_identity = ATOMIC64_INIT(0);

static void cred_init_root(struct cred *cred)
{
	memset(cred, 0, sizeof(*cred));
	refcount_set(&cred->refs, 1);
}

struct cred *cred_alloc_root(void)
{
	struct cred *cred = kzalloc(sizeof(*cred), ALLOC_NOWAIT);

	if (cred)
		cred_init_root(cred);
	return cred;
}

struct cred *cred_dup(const struct cred *source)
{
	struct cred *cred = kzalloc(sizeof(*cred), ALLOC_NOWAIT);

	if (!cred)
		return NULL;
	if (source)
		memcpy(cred, source, sizeof(*cred));
	else
		cred_init_root(cred);
	refcount_set(&cred->refs, 1);
	return cred;
}

void cred_get(struct cred *cred)
{
	if (cred)
		refcount_inc(&cred->refs);
}

void cred_put(struct cred *cred)
{
	if (cred && refcount_dec_and_test(&cred->refs))
		kfree(cred);
}

static void cred_replace(struct task_struct *task, struct cred *cred)
{
	struct cred *old;
	irq_flags_t flags;

	/* Serialize the immutable credential binding with remote snapshots. */
	spin_lock_irqsave(&task->lock, &flags);
	old = task->cred;
	task->cred = cred;
	spin_unlock_irqrestore(&task->lock, flags);
	cred_put(old);
}

struct cred *task_cred_get(struct task_struct *task)
{
	struct cred *cred;
	irq_flags_t flags;

	if (!task || task_is_idle(task))
		return NULL;
	spin_lock_irqsave(&task->lock, &flags);
	cred = task->cred;
	cred_get(cred);
	spin_unlock_irqrestore(&task->lock, flags);
	return cred;
}

static bool cred_uid_match(const struct cred *a, const struct cred *b)
{
	return a->ruid == b->ruid || a->ruid == b->euid ||
	       a->ruid == b->suid || a->euid == b->ruid ||
	       a->euid == b->euid || a->euid == b->suid ||
	       a->suid == b->ruid || a->suid == b->euid ||
	       a->suid == b->suid;
}

static bool cred_gid_match(const struct cred *a, const struct cred *b)
{
	return a->rgid == b->rgid || a->rgid == b->egid ||
	       a->rgid == b->sgid || a->egid == b->rgid ||
	       a->egid == b->egid || a->egid == b->sgid ||
	       a->sgid == b->rgid || a->sgid == b->egid ||
	       a->sgid == b->sgid;
}

int task_access_check(struct task_struct *caller, struct task_struct *target,
			      enum task_access_mode mode)
{
	struct cred *caller_cred __cleanup_with(cred_ref) = NULL;
	struct cred *target_cred __cleanup_with(cred_ref) = NULL;
	bool allowed;

	if (!caller || !target)
		return -ESRCH;
	if (caller == target)
		return 0;
	if (mode != TASK_ACCESS_SCHEDULER_WRITE)
		return -EINVAL;
	caller_cred = task_cred_get(caller);
	target_cred = task_cred_get(target);
	if (!caller_cred || !target_cred) {
		return -EPERM;
	}
	allowed = caller_cred->ruid == 0 || caller_cred->euid == 0 ||
		  caller_cred->suid == 0 || cred_uid_match(caller_cred, target_cred) ||
		  cred_gid_match(caller_cred, target_cred);
	return allowed ? 0 : -EPERM;
}

int task_set_uid(struct task_struct *task, uid_t uid)
{
	struct cred *source __cleanup_with(cred_ref) = task_cred_get(task);
	struct cred *cred;

	if (!source)
		return -EINVAL;
	cred = cred_dup(source);
	if (!cred)
		return -ENOMEM;
	cred->ruid = uid;
	cred->euid = uid;
	cred->suid = uid;
	cred->fsuid = uid;
	cred_replace(task, cred);
	return 0;
}

int task_set_gid(struct task_struct *task, gid_t gid)
{
	struct cred *source __cleanup_with(cred_ref) = task_cred_get(task);
	struct cred *cred;

	if (!source)
		return -EINVAL;
	cred = cred_dup(source);
	if (!cred)
		return -ENOMEM;
	cred->rgid = gid;
	cred->egid = gid;
	cred->sgid = gid;
	cred->fsgid = gid;
	cred_replace(task, cred);
	return 0;
}

int task_set_groups(struct task_struct *task, const gid_t *groups,
			    uint32_t ngroups)
{
	struct cred *source __cleanup_with(cred_ref) = task_cred_get(task);
	struct cred *cred;

	if (!source)
		return -EINVAL;
	if (ngroups > NGROUPS_MAX || (ngroups != 0 && !groups)) {
		return -EINVAL;
	}
	cred = cred_dup(source);
	if (!cred)
		return -ENOMEM;
	if (ngroups > 0)
		memcpy(cred->groups, groups, ngroups * sizeof(gid_t));
	cred->ngroups = ngroups;
	cred_replace(task, cred);
	return 0;
}

static void task_init_control(struct task_struct *task)
{
	spin_lock_init(&task->lock, LOCK_RANK_WAIT, LOCK_IRQ_HARDIRQ_REACHABLE);
	task->wait.owner = task;
	INIT_LIST_HEAD(&task->wait.deadline_node);
}

static void task_init_common(struct task_struct *task)
{
	memset(task, 0, sizeof(*task));
	task->identity =
		(uint64_t)atomic64_add_fetch_relaxed(&next_task_identity, 1);
	BUG_ON(!task->identity);
	refcount_set(&task->refs, 1);
	task->lifecycle = TASK_NEW;
	task->exit_request = TASK_EXIT_REQUEST_NONE;
	task->run_state = TASK_DORMANT;
	sched_task_init(task);
	INIT_LIST_HEAD(&task->proc_node);
	INIT_LIST_HEAD(&task->retired_node);
	task_init_control(task);
	task->signal.sas.ss_flags = SS_DISABLE;
	arch_task_init(task);
}

struct task_struct *task_alloc(void)
{
	struct task_struct *task;
	struct pid_identity *tid;
	void *kstack;

	task = kzalloc(sizeof(*task), ALLOC_NOWAIT);
	if (!task)
		return NULL;
	kstack = get_free_page(KSTACK_ORDER, ALLOC_NOWAIT);
	if (!kstack)
		goto fail_task;
	tid = pid_alloc();
	if (!tid)
		goto fail_stack;
	task_init_common(task);
	task->tid = tid;
	task->cred = cred_alloc_root();
	if (!task->cred)
		goto fail_tid;
	task->arch.kstack = kstack;
	memset(kstack, 0, KSTACK_SIZE);
	return task;

fail_tid:
	pid_put(tid);
fail_stack:
	free_page(kstack, KSTACK_ORDER);
fail_task:
	kfree(task);
	return NULL;
}

struct mm_struct *task_replace_mm(struct task_struct *task, struct mm_struct *mm)
{
	struct mm_struct *old;
	irq_flags_t flags;

	if (mm)
		mm_publish(mm);
	spin_lock_irqsave(&task->lock, &flags);
	old = task->mm;
	task->mm = mm;
	spin_unlock_irqrestore(&task->lock, flags);
	if (old)
		mm_unpublish(old);
	return old;
}

struct mm_struct *task_mm_get(struct task_struct *task)
{
	struct mm_struct *mm;
	irq_flags_t flags;

	if (!task)
		return NULL;
	spin_lock_irqsave(&task->lock, &flags);
	mm = task->mm;
	mm_get(mm);
	spin_unlock_irqrestore(&task->lock, flags);
	return mm;
}

int task_prepare_kernel(struct task_struct *task)
{
	if (!task || task->proc)
		return -EINVAL;
	return 0;
}

static int task_init_file_context(struct task_struct *task)
{
	struct files_struct *files;
	struct fs_struct *fs;

	if (task->files && task->fs)
		return 0;
	BUG_ON(task->files || task->fs);
	files = files_alloc();
	if (!files)
		return -ENOMEM;
	fs = fs_alloc();
	if (!fs) {
		files_put(files);
		return -ENOMEM;
	}
	files_install_standard_fds(files);
	task->files = files;
	task->fs = fs;
	return 0;
}

void task_release_file_context(struct task_struct *task)
{
	struct files_struct *files = task->files;
	struct fs_struct *fs = task->fs;

	task->files = NULL;
	task->fs = NULL;
	files_put(files);
	fs_put(fs);
}

int task_prepare_user_proc(struct task_struct *task, struct proc_struct *proc)
{
	int ret;

	if (!task || !proc || task->proc)
		return -EINVAL;
	ret = proc_attach_task(proc, task, true);
	if (ret < 0)
		return ret;
	if (!proc->sighand && (ret = proc_init_resources(proc)) < 0) {
		proc_detach_task(proc, task);
		return ret;
	}
	ret = task_init_resources(task);
	if (ret < 0) {
		task_release_file_context(task);
		proc_detach_task(proc, task);
		return ret;
	}
	return 0;
}

int task_create_initial_proc(struct task_struct *task)
{
	struct proc_struct *proc;
	int ret;

	if (!task || task->proc || !task->tid)
		return -EINVAL;
	proc = proc_alloc(NULL, task->tid);
	if (!proc)
		return -ENOMEM;
	ret = proc_attach_task(proc, task, true);
	if (ret < 0)
		goto fail_proc;
	ret = proc_init_resources(proc);
	if (ret < 0)
		goto fail_task;
	ret = task_init_resources(task);
	if (ret < 0)
		goto fail_resources;
	ret = proc_publish(proc);
	if (ret < 0)
		goto fail_resources;
	proc_put(proc);
	return 0;

fail_resources:
	task_release_file_context(task);
	proc_release_resources(proc);
fail_task:
	proc_detach_task(proc, task);
	proc->lifecycle = PROC_DEAD;
fail_proc:
	proc_put(proc);
	return ret;
}

int task_init_resources(struct task_struct *task)
{
	int ret;

	if (!task)
		return -EINVAL;
	if (!task->proc)
		return 0;
	ret = task_init_file_context(task);
	if (ret < 0)
		return ret;
	return sig_task_init(task);
}

void task_release_resources(struct task_struct *task)
{
	struct cred *cred;
	irq_flags_t flags;

	if (!task)
		return;
	task_release_file_context(task);
	mm_put(task_replace_mm(task, NULL));
	kernel_clone_complete_vfork(task);
	sig_task_release(task);
	spin_lock_irqsave(&task->lock, &flags);
	cred = task->cred;
	task->cred = NULL;
	spin_unlock_irqrestore(&task->lock, flags);
	cred_put(cred);
}

bool task_begin_exit(struct task_struct *task)
{
	struct signal_struct *signal;
	bool begun = false;
	irq_flags_t flags;

	if (!task || task_is_idle(task))
		return false;
	signal = task->proc ? &task->proc->signal : NULL;
	if (signal)
		return sig_task_begin_exit(task);
	spin_lock_irqsave(&task->lock, &flags);
	if (task->lifecycle == TASK_LIVE) {
		task->lifecycle = TASK_EXITING;
		begun = true;
	}
	spin_unlock_irqrestore(&task->lock, flags);
	return begun;
}

static void task_kick_exit(struct task_struct *task)
{
	(void)wait_wake_exit(task);
}

bool task_request_exec_exit(struct task_struct *task)
{
	bool requested = false;
	irq_flags_t flags;

	if (!task || task_is_idle(task) || task == current_task())
		return false;

	spin_lock_irqsave(&task->lock, &flags);
	if (task->lifecycle == TASK_LIVE) {
		task->exit_request = TASK_EXIT_REQUEST_EXEC;
		requested = true;
	}
	spin_unlock_irqrestore(&task->lock, flags);
	if (!requested)
		return task_exec_exit_requested(task);

	/* A sibling must reach user_return_work in its own context. */
	task_kick_exit(task);
	return true;
}

bool task_request_group_exit(struct task_struct *task)
{
	irq_flags_t flags;
	bool live;

	if (!task || task_is_idle(task) || task == current_task())
		return false;
	spin_lock_irqsave(&task->lock, &flags);
	live = task->lifecycle == TASK_LIVE;
	spin_unlock_irqrestore(&task->lock, flags);
	if (live)
		task_kick_exit(task);
	return live;
}

bool task_exec_exit_requested(struct task_struct *task)
{
	bool requested;
	irq_flags_t flags;

	if (!task)
		return false;
	spin_lock_irqsave(&task->lock, &flags);
	requested = task->exit_request == TASK_EXIT_REQUEST_EXEC;
	spin_unlock_irqrestore(&task->lock, flags);
	return requested;
}

void task_mark_dead(struct task_struct *task)
{
	irq_flags_t flags;

	if (!task)
		return;
	/* Paired with the locked lifecycle read in task_try_get_live. */
	spin_lock_irqsave(&task->lock, &flags);
	BUG_ON(task->lifecycle != TASK_EXITING);
	task->lifecycle = TASK_DEAD;
	spin_unlock_irqrestore(&task->lock, flags);
}

bool task_reap_ready(const struct task_struct *task)
{
	/* Only the reaper calls this, after sched_retired_pop(): the retired
	 * queue lock transfer orders every field written before the push
	 * (lifecycle, on_rq, wait teardown, proc detach) with this read. */
	return task && !task_is_idle(task) && task != current_task() &&
	       task->lifecycle == TASK_DEAD &&
	       task->run_state == TASK_RETIRED && task->wait.phase == WAIT_IDLE &&
	       task->wait.registration_count == 0 &&
	       !task->wait.deadline_queued && !task->wait.deadline_task;
}

static void task_destroy(struct task_struct *task)
{
	void *kstack;

	BUG_ON(!task || task_is_idle(task));
	BUG_ON(task == current_task());
	BUG_ON(task->published);
	BUG_ON(task->lifecycle != TASK_DEAD);
	BUG_ON(task->run_state != TASK_RETIRED &&
	       task->run_state != TASK_DORMANT);
	BUG_ON(task->wait.registration_count != 0);
	BUG_ON(!list_empty(&task->proc_node));
	task_release_resources(task);
	proc_put(task->proc);
	task->proc = NULL;
	kstack = task_kernel_stack_take(task);
	BUG_ON(!kstack);
	free_page(kstack, KSTACK_ORDER);
	pid_put(task->tid);
	kfree(task);
}

void task_free(struct task_struct *task)
{
	if (!task)
		return;
	BUG_ON(task->published);
	BUG_ON(refcount_read(&task->refs) != 1);
	if (task->proc)
		proc_detach_task(task->proc, task);
	task->lifecycle = TASK_DEAD;
	task_destroy(task);
}

int task_publish(struct task_struct *task)
{
	int ret;

	BUG_ON(!task || task->lifecycle != TASK_NEW || !task->tid);
	if (task->proc)
		ret = proc_publish_with_task(task->proc, task);
	else {
		task->lifecycle = TASK_LIVE;
		ret = pid_publish_task(task->tid, task);
		if (!ret) {
			task->published = true;
			sched_enqueue_new(task);
		}
	}
	if (ret)
		task->lifecycle = TASK_NEW;
	return ret;
}

void task_unpublish(struct task_struct *task)
{
	struct pid_identity *tid;

	if (!task || !task->published)
		return;
	task->published = false;
	tid = task->tid;
	pid_unpublish_task(tid, task);
}

void task_reap_unpublish(struct task_struct *task)
{
	if (!task || task_is_idle(task))
		return;
	BUG_ON(task == current_task());
	BUG_ON(task->lifecycle != TASK_DEAD);
	task_unpublish(task);
}

bool task_try_get(struct task_struct *task)
{
	return task &&
	       (task_is_idle(task) || refcount_inc_not_zero(&task->refs));
}

bool task_try_get_live(struct task_struct *task)
{
	irq_flags_t flags;
	bool live;

	/* Idle tasks have no refcount and are always live. */
	if (task_is_idle(task))
		return true;
	if (!task_try_get(task))
		return false;
	spin_lock_irqsave(&task->lock, &flags);
	live = task->lifecycle == TASK_LIVE;
	spin_unlock_irqrestore(&task->lock, flags);
	if (!live)
		task_put(task);
	return live;
}

static DEFINE_SPINLOCK(disposal_lock, LOCK_RANK_RETIRED,
		       LOCK_IRQ_HARDIRQ_REACHABLE);
static LIST_HEAD(disposals);

void task_reap_deferred(void)
{
	for (;;) {
		struct task_struct *task;
		irq_flags_t flags;

		spin_lock_irqsave(&disposal_lock, &flags);
		if (list_empty(&disposals)) {
			spin_unlock_irqrestore(&disposal_lock, flags);
			return;
		}
		task = list_first_entry(&disposals, struct task_struct, retired_node);
		list_del_init(&task->retired_node);
		spin_unlock_irqrestore(&disposal_lock, flags);
		task_destroy(task);
	}
}

void task_put(struct task_struct *task)
{
	if (!task || task_is_idle(task))
		return;
	if (!refcount_dec_and_test(&task->refs))
		return;
	if (wait_may_block()) {
		task_destroy(task);
	} else {
		irq_flags_t flags;

		spin_lock_irqsave(&disposal_lock, &flags);
		list_add_tail(&task->retired_node, &disposals);
		spin_unlock_irqrestore(&disposal_lock, flags);
		sched_notify_reaper();
	}
}

void task_init(void)
{
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		struct task_struct *idle = idle_tasks + id;

		task_init_common(idle);
		idle->flags |= TASK_FLAG_IDLE;
		idle->lifecycle = TASK_LIVE;
		idle->run_state = TASK_RUNNING;
		/* The boot context runs on the physical idle stack; keep bounds
		 * and runtime stack in the same address space. */
		idle->arch.kstack = (void *)__pa(idle_stacks[id]);
		/* Idle Tasks are never enqueued, so their CPU assignment is
		 * fixed at construction time; the topology is already
		 * published by smp_prepare(). */
		if (id < nr_cpu_ids)
			idle->cpu = &cpu_table[id];
	}
	cpu_boot_init(idle_tasks);
	set_current_task(idle_tasks);
	pid_init();
	wait_init();
	pr_debug("task: idle task created\n");
}

struct task_struct *kernel_thread(void (*fn)(void *), void *arg)
{
	struct task_struct *task = task_alloc();

	if (!task)
		return NULL;
	if (task_prepare_kernel(task) < 0) {
		task_free(task);
		return NULL;
	}
	task_setup_kthread(task, fn, arg);
	if (task_publish(task) < 0) {
		task_free(task);
		return NULL;
	}
	return task;
}

void set_init_task(struct task_struct *task)
{
	BUG_ON(!task);
	BUG_ON(init_task && init_task != task);
	init_task = task;
}

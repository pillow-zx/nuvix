/*
 * kernel/task.c - schedulable task object and task-local lifecycle
 */

#include <kernel/buddy.h>
#include <kernel/errno.h>
#include <kernel/fdtable.h>
#include <kernel/fs_struct.h>
#include <kernel/mm.h>
#include <kernel/pid.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/slab.h>
#include <kernel/task.h>
#include <kernel/vfs.h>

struct task_struct idle_tasks[NR_CPUS];
uint8_t idle_stacks[NR_CPUS][KSTACK_SIZE] __aligned(PAGE_SIZE);
struct cpu cpu_table[NR_CPUS];
uint32_t nr_cpu_ids;
struct task_struct *init_task;

/* CPU masks. Publication is release, observation is acquire; the scheduler
 * and syscall layers query only through cpu_online_mask()/cpu_schedulable_mask().
 */
static atomic64_t online_cpu_mask;
static atomic64_t schedulable_cpu_mask;

uint64_t cpu_online_mask(void)
{
	return (uint64_t)atomic64_read_acquire(&online_cpu_mask);
}

uint64_t cpu_schedulable_mask(void)
{
	return (uint64_t)atomic64_read_acquire(&schedulable_cpu_mask);
}

void cpu_set_online(uint32_t id)
{
	atomic64_or_fetch_release(&online_cpu_mask, (int64_t)(1ULL << id));
}

void cpu_set_schedulable(uint32_t id)
{
	atomic64_or_fetch_release(&schedulable_cpu_mask, (int64_t)(1ULL << id));
}

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

int task_set_uid(struct task_struct *task, uid_t uid)
{
	struct cred *old;
	struct cred *cred;

	if (!task || !task->cred)
		return -EINVAL;
	cred = cred_dup(task->cred);
	if (!cred)
		return -ENOMEM;
	cred->ruid = uid;
	cred->euid = uid;
	cred->suid = uid;
	cred->fsuid = uid;
	old = task->cred;
	task->cred = cred;
	cred_put(old);
	return 0;
}

int task_set_gid(struct task_struct *task, gid_t gid)
{
	struct cred *old;
	struct cred *cred;

	if (!task || !task->cred)
		return -EINVAL;
	cred = cred_dup(task->cred);
	if (!cred)
		return -ENOMEM;
	cred->rgid = gid;
	cred->egid = gid;
	cred->sgid = gid;
	cred->fsgid = gid;
	old = task->cred;
	task->cred = cred;
	cred_put(old);
	return 0;
}

int task_set_groups(struct task_struct *task, const gid_t *groups,
		    uint32_t ngroups)
{
	struct cred *old;
	struct cred *cred;

	if (!task || !task->cred)
		return -EINVAL;
	if (ngroups > NGROUPS_MAX)
		return -EINVAL;
	cred = cred_dup(task->cred);
	if (!cred)
		return -ENOMEM;
	if (ngroups > 0)
		memcpy(cred->groups, groups, ngroups * sizeof(gid_t));
	cred->ngroups = ngroups;
	old = task->cred;
	task->cred = cred;
	cred_put(old);
	return 0;
}

static void task_init_wait(struct task_struct *task)
{
	spin_lock_init(&task->wait.lock, LOCK_RANK_WAIT);
	task->wait.policy = TASK_WAIT_UNINTERRUPTIBLE;
	task->wait.reason = TASK_WAKE_NONE;
	task->wait.generation = 0;
	task->wait.status = WAIT_IDLE;
	task->wait.status_value = 0;
	task->wait.owner = NULL;
	task->wait.deadline_queued = false;
	task->wait.deadline_cpu = 0;
	task->wait.deadline_generation = 0;
	task->wait.deadline_task = NULL;
	INIT_LIST_HEAD(&task->wait.registrations);
	INIT_LIST_HEAD(&task->wait.deadline_node);
	for (uint32_t i = 0; i < WAIT_MAX_REGISTRATIONS; i++) {
		INIT_LIST_HEAD(&task->wait.entries[i].channel_node);
		INIT_LIST_HEAD(&task->wait.entries[i].registration_node);
	}
	task->wait.deadline = wait_deadline_none();
}

static void task_init_common(struct task_struct *task)
{
	memset(task, 0, sizeof(*task));
	refcount_set(&task->refs, 1);
	task->lifecycle = TASK_NEW;
	task->exit_request = TASK_EXIT_REQUEST_NONE;
	task->run_state = TASK_RUNNABLE;
	task->allowed_cpus = SCHED_BOOT_AFFINITY_MASK;
	task->cpu = NULL;
	task->on_rq = false;
	task->on_cpu = false;
	INIT_LIST_HEAD(&task->proc_node);
	INIT_LIST_HEAD(&task->retired_node);
	task->reap_proc = NULL;
	task_init_wait(task);
	atomic_set(&task->sched.need_resched, 0);
	INIT_LIST_HEAD(&task->sched.run_node);
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
	sched_task_init(task);
	return task;

fail_tid:
	pid_put(tid);
fail_stack:
	free_page(kstack, KSTACK_ORDER);
fail_task:
	kfree(task);
	return NULL;
}

int task_prepare_kernel(struct task_struct *task)
{
	if (!task || task->proc)
		return -EINVAL;
	return 0;
}

int task_prepare_user_proc(struct task_struct *task, struct proc_struct *proc)
{
	int ret;

	if (!task || !proc || task->proc)
		return -EINVAL;
	ret = proc_attach_task(proc, task, true);
	if (ret < 0)
		return ret;
	if (!proc->files && (ret = proc_init_resources(proc)) < 0) {
		proc_detach_task(proc, task);
		return ret;
	}
	ret = signals_init(task);
	if (ret < 0) {
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
	ret = signals_init(task);
	if (ret < 0)
		goto fail_resources;
	ret = proc_publish(proc);
	if (ret < 0)
		goto fail_resources;
	proc_put(proc);
	return 0;

fail_resources:
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
	if (!task)
		return -EINVAL;
	if (!task->proc)
		return 0;
	if (!task->proc->files) {
		int ret = proc_init_resources(task->proc);

		if (ret < 0)
			return ret;
	}
	return signals_init(task);
}

void task_release_resources(struct task_struct *task)
{
	if (!task)
		return;
	signals_release(task);
	cred_put(task->cred);
	task->cred = NULL;
}

static bool task_active_on_cpu(const struct task_struct *task)
{
	if (!task)
		return false;
	for (uint32_t id = 0; id < nr_cpu_ids; id++)
		if (cpu_current_task(cpu_by_id(id)) == task)
			return true;
	return false;
}

bool task_begin_exit(struct task_struct *task)
{
	bool begun = false;
	irq_flags_t flags;

	if (!task || task_is_idle(task))
		return false;
	spin_lock_irqsave(&task->wait.lock, &flags);
	if (task->lifecycle == TASK_LIVE) {
		task->lifecycle = TASK_EXITING;
		begun = true;
	}
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return begun;
}

static void task_kick_exit(struct task_struct *task)
{
	(void)wait_wake_exit(task);
	(void)sched_resume(task);
	(void)sched_wake_external(task);
}

bool task_request_exec_exit(struct task_struct *task)
{
	bool requested = false;
	irq_flags_t flags;

	if (!task || task_is_idle(task) || task == current_task())
		return false;

	spin_lock_irqsave(&task->wait.lock, &flags);
	if (task->lifecycle == TASK_LIVE) {
		task->exit_request = TASK_EXIT_REQUEST_EXEC;
		requested = true;
	}
	spin_unlock_irqrestore(&task->wait.lock, flags);
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
	spin_lock_irqsave(&task->wait.lock, &flags);
	live = task->lifecycle == TASK_LIVE;
	spin_unlock_irqrestore(&task->wait.lock, flags);
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
	spin_lock_irqsave(&task->wait.lock, &flags);
	requested = task->exit_request == TASK_EXIT_REQUEST_EXEC;
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return requested;
}

void task_mark_dead(struct task_struct *task)
{
	irq_flags_t flags;

	if (!task)
		return;
	/* Paired with the locked lifecycle read in task_try_get_live. */
	spin_lock_irqsave(&task->wait.lock, &flags);
	BUG_ON(task->lifecycle != TASK_EXITING);
	task->lifecycle = TASK_DEAD;
	spin_unlock_irqrestore(&task->wait.lock, flags);
}

bool task_reap_ready(const struct task_struct *task)
{
	return task && !task_is_idle(task) && task != current_task() &&
	       task->lifecycle == TASK_DEAD && !task->on_rq &&
	       !task_active_on_cpu(task) && task->wait.status == WAIT_IDLE &&
	       list_empty(&task->wait.registrations) &&
	       !task->wait.deadline_queued && !task->wait.deadline_task &&
	       list_empty(&task->proc_node);
}

static void task_destroy(struct task_struct *task)
{
	void *kstack;

	BUG_ON(!task || task_is_idle(task));
	BUG_ON(task == current_task());
	BUG_ON(task->published);
	BUG_ON(task->lifecycle != TASK_DEAD);
	BUG_ON(task->on_rq || task_active_on_cpu(task));
	BUG_ON(!list_empty(&task->wait.registrations));
	BUG_ON(!list_empty(&task->proc_node));
	BUG_ON(task->reap_proc);
	task_release_resources(task);
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

void task_publish(struct task_struct *task)
{
	int ret;

	BUG_ON(!task || task->lifecycle != TASK_NEW || !task->tid);
	if (task->proc && !task->proc->published) {
		task->lifecycle = TASK_LIVE;
		ret = proc_publish_with_task(task->proc, task);
		if (ret < 0)
			task->lifecycle = TASK_NEW;
	} else {
		task->lifecycle = TASK_LIVE;
		ret = pid_publish_task(task->tid, task);
		if (ret < 0)
			task->lifecycle = TASK_NEW;
	}
	BUG_ON(ret < 0);
	task->published = true;
	task->run_state = TASK_RUNNABLE;
}

void task_unpublish(struct task_struct *task)
{
	struct pid_identity *tid;

	if (!task || !task->published)
		return;
	task->published = false;
	tid = task->tid;
	task->tid = NULL;
	pid_unpublish_task(tid, task);
	pid_put(tid);
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
	/* Lifecycle transitions (task_begin_exit, task_mark_dead) happen
	 * under task->wait.lock; read it under the same lock. Lock order:
	 * pid_lock (20) then wait.lock (40), ascending. */
	spin_lock_irqsave(&task->wait.lock, &flags);
	live = task->lifecycle == TASK_LIVE;
	spin_unlock_irqrestore(&task->wait.lock, flags);
	if (!live)
		task_put(task);
	return live;
}

void task_put(struct task_struct *task)
{
	if (!task || task_is_idle(task))
		return;
	if (refcount_dec_and_test(&task->refs))
		task_destroy(task);
}

int cpu_topology_init(const struct cpu_topology_entry *entries, uint32_t count)
{
	uint32_t i;
	uint32_t j;

	if (!entries || !count || count > NR_CPUS)
		return -EINVAL;
	/* Validate the whole set before touching any slot: unique logical IDs
	 * within array capacity and unique hart IDs. */
	for (i = 0; i < count; i++) {
		if (entries[i].logical_id >= NR_CPUS)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			if (entries[j].logical_id == entries[i].logical_id ||
			    entries[j].hartid == entries[i].hartid)
				return -EINVAL;
		}
	}
	for (i = 0; i < count; i++) {
		struct cpu *cpu = &cpu_table[entries[i].logical_id];

		cpu->id = entries[i].logical_id;
		cpu->hartid = entries[i].hartid;
		atomic_init(&cpu->state, CPU_OFFLINE);
	}
	nr_cpu_ids = count;
	return 0;
}

void cpu_boot_init(struct task_struct *idles)
{
	BUG_ON(!idles);
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		struct cpu *cpu = &cpu_table[id];

		cpu->flags = 0;
		cpu->idle_task = &idles[id];
		cpu->current_task = NULL;
		cpu->preempt_count = 0;
		cpu->irq_nesting = 0;
		cpu->lock_depth = 0;
#ifdef CONFIG_DEBUG_CONTEXT
		for (uint32_t lock = 0; lock < CPU_LOCK_MAX; lock++) {
			cpu->locks[lock] = NULL;
			cpu->lock_flags[lock] = 0;
			cpu->lock_irqsave[lock] = false;
		}
#endif
	}
	cpu_table[0].current_task = idles;
	cpu_state_store_release(&cpu_table[0], CPU_ONLINE);
	cpu_set_online(0);
	cpu_set_schedulable(0);
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
	}
	cpu_boot_init(idle_tasks);
	set_current_task(idle_tasks);
	pid_init();
	wait_init();
	pr_info("task: idle task created\n");
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
	task_publish(task);
	sched_enqueue_new(task);
	return task;
}

void set_init_task(struct task_struct *task)
{
	BUG_ON(!task);
	BUG_ON(init_task && init_task != task);
	init_task = task;
}

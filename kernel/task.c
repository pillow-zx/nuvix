/*
 * kernel/task.c - 进程控制块管理
 */

#include <kernel/task.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/pid.h>
#include <kernel/slab.h>
#include <kernel/buddy.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/mutex.h>
#include <kernel/fdtable.h>
#include <kernel/fs_struct.h>
#include <kernel/vfs.h>

#include "pid_internal.h"
#include "task_internal.h"

struct task_pgid_query {
	pid_t pgid;
	pid_t sid;
	const struct task_struct *ignored;
};

struct task_child_event_record {
	struct list_head node;
	struct task_struct *claimer;
	uint64_t sequence;
	enum task_child_event_type type;
	int status;
};

struct task_child_notification {
	struct task_struct *parent;
	pid_t pid;
	uid_t uid;
	int code;
	int status;
	bool send_sigchld;
	bool parent_ref;
	bool wake_parent;
};

struct task_child_wakeup {
	struct task_struct *parent;
	bool parent_ref;
	bool pending;
};

struct task_struct idle_task;

struct cpu cpu_table[NR_CPUS];
uint32_t nr_cpu_ids;

struct task_struct *init_task;
static DEFINE_MUTEX(process_lock);
static DEFINE_SPINLOCK(child_relation_lock);

static void task_child_lock(irq_flags_t *flags)
{
	spin_lock_irqsave(&child_relation_lock, flags);
}

static void task_child_unlock(irq_flags_t flags)
{
	spin_unlock_irqrestore(&child_relation_lock, flags);
}

static int child_exit_si_code(int status)
{
	return (status & 0x7f) == 0 ? CLD_EXITED : CLD_KILLED;
}

static int child_exit_si_status(int status)
{
	return (status & 0x7f) == 0 ? (status >> 8) & 0xff : status & 0x7f;
}

static int child_stop_status(int sig)
{
	return (sig << 8) | 0x7f;
}

static struct task_child_event_record *
task_child_event_alloc(enum task_child_event_type type, int status)
{
	struct task_child_event_record *event =
		kzalloc(sizeof(*event), ALLOC_NOWAIT);

	BUG_ON(!event);
	INIT_LIST_HEAD(&event->node);
	event->type = type;
	event->status = status;
	return event;
}

static void task_child_event_free_list(struct list_head *events)
{
	struct list_head *pos;
	struct list_head *next;

	list_for_each_safe (pos, next, events) {
		struct task_child_event_record *event =
			list_entry(pos, struct task_child_event_record, node);

		list_del_init(&event->node);
		kfree(event);
	}
}

static void task_child_event_discard_all_locked(struct task_struct *task,
						struct list_head *discarded)
{
	struct list_head *pos;
	struct list_head *next;

	list_for_each_safe (pos, next, &task->lifecycle.child_events) {
		list_del_init(pos);
		list_add_tail(pos, discarded);
	}
}

static void task_child_event_discard_all(struct task_struct *task)
{
	LIST_HEAD(discarded);
	irq_flags_t flags;

	task_child_lock(&flags);
	task_child_event_discard_all_locked(task, &discarded);
	task_child_unlock(flags);
	task_child_event_free_list(&discarded);
}

static bool task_child_matches(const struct task_struct *child, pid_t pid)
{
	return task_is_group_leader(child) &&
	       (pid == (pid_t)-1 || task_pid(child) == pid);
}

static bool task_child_has_target_locked(const struct task_struct *parent,
					 pid_t pid)
{
	struct task_struct *child;

	list_for_each_entry (child, &parent->links.children, links.sibling) {
		if (task_child_matches(child, pid))
			return true;
	}

	return false;
}

static struct task_child_event_record *
task_child_event_find_locked(struct task_struct *child, uint32_t event_mask)
{
	struct task_child_event_record *event;

	list_for_each_entry (event, &child->lifecycle.child_events, node) {
		if (event->claimer || !(event_mask & (1u << event->type)))
			continue;
		return event;
	}

	return NULL;
}

static struct task_child_event_record *
task_child_event_find_sequence_locked(struct task_struct *child,
				      uint64_t sequence)
{
	struct task_child_event_record *event;

	list_for_each_entry (event, &child->lifecycle.child_events, node) {
		if (event->sequence == sequence)
			return event;
	}

	return NULL;
}

static enum task_child_wait_state
task_child_event_observe_locked(struct task_struct *parent, pid_t pid,
				uint32_t event_mask,
				struct task_struct **matched_child,
				struct task_child_event_record **matched_event)
{
	struct task_struct *child;

	if (matched_child)
		*matched_child = NULL;
	if (matched_event)
		*matched_event = NULL;

	list_for_each_entry (child, &parent->links.children, links.sibling) {
		struct task_child_event_record *event;

		if (!task_child_matches(child, pid))
			continue;
		event = task_child_event_find_locked(child, event_mask);
		if (!event)
			continue;
		if (matched_child)
			*matched_child = child;
		if (matched_event)
			*matched_event = event;
		return TASK_CHILD_WAIT_EVENT;
	}

	return task_child_has_target_locked(parent, pid)
		       ? TASK_CHILD_WAIT_NO_EVENT
		       : TASK_CHILD_WAIT_NO_CHILD;
}

static void task_child_event_clear_claims_locked(struct task_struct *child)
{
	struct task_child_event_record *event;

	list_for_each_entry (event, &child->lifecycle.child_events, node)
		event->claimer = NULL;
}

static void task_reparent_children_locked(struct task_struct *dead,
					  struct task_child_wakeup *wakeup)
{
	struct list_head *pos;
	struct list_head *next;
	struct task_struct *parent = init_task ? init_task : &idle_task;

	list_for_each_safe (pos, next, &dead->links.children) {
		struct task_struct *child =
			list_entry(pos, struct task_struct, links.sibling);

		list_del_init(&child->links.sibling);
		child->links.parent = parent;
		list_add_tail(&child->links.sibling,
			      &parent->links.children);
		task_child_event_clear_claims_locked(child);
		if (!list_empty(&child->lifecycle.child_events))
			wakeup->pending = true;
	}

	if (wakeup->pending && task_try_get(parent)) {
		wakeup->parent = parent;
		wakeup->parent_ref = parent != &idle_task;
	} else if (wakeup->pending) {
		wakeup->pending = false;
	}
}

static void
task_child_event_publish_locked(struct task_struct *child,
				struct task_child_event_record *event, int code,
				int signal_status,
				struct task_child_notification *notification)
{
	struct task_struct *parent = child->links.parent;

	event->sequence = ++child->lifecycle.next_child_event_sequence;
	BUG_ON(event->sequence == 0);
	list_add_tail(&event->node, &child->lifecycle.child_events);
	memset(notification, 0, sizeof(*notification));
	if (!parent)
		return;
	if (!task_try_get(parent))
		return;

	notification->parent = parent;
	notification->parent_ref = parent != &idle_task;
	notification->wake_parent = true;
	notification->pid = task_pid(child);
	notification->uid = task_uid(child);
	notification->code = code;
	notification->status = signal_status;
	notification->send_sigchld = task_exit_signal(child) == SIGCHLD;
}

static void task_child_event_send_notification(
	const struct task_child_notification *notification)
{
	siginfo_t info = {0};

	if (notification->wake_parent)
		wait_channel_wake_one(&notification->parent->links.wait_child_queue);

	if (!notification->send_sigchld)
		goto out;

	info.si_signo = SIGCHLD;
	info.si_code = notification->code;
	info.si_pid = notification->pid;
	info.si_uid = notification->uid;
	info.si_status = notification->status;
	(void)send_signal_info(SIGCHLD, &info, notification->parent);

out:
	if (notification->parent_ref)
		task_put(notification->parent);
}

static void task_child_wake_parent(struct task_child_wakeup *wakeup)
{
	if (!wakeup || !wakeup->pending)
		return;

	wait_channel_wake_all(&wakeup->parent->links.wait_child_queue);
	if (wakeup->parent_ref)
		task_put(wakeup->parent);
	wakeup->pending = false;
}

void cpu_boot_init(struct task_struct *idle)
{
	BUG_ON(!idle);

	for (uint32_t id = 0; id < NR_CPUS; id++) {
		cpu_table[id].id = id;
		cpu_table[id].hartid = id;
		cpu_table[id].state = CPU_OFFLINE;
		cpu_table[id].flags = 0;
		cpu_table[id].idle_task = NULL;
		cpu_table[id].current_task = NULL;
		cpu_table[id].preempt_count = 0;
		cpu_table[id].irq_nesting = 0;
		cpu_table[id].lock_depth = 0;
		cpu_table[id].lock_irq_flags = 0;
#ifdef CONFIG_DEBUG_CONTEXT
		for (uint32_t lock = 0; lock < CPU_LOCK_MAX; lock++)
			cpu_table[id].locks[lock] = NULL;
#endif
	}

	nr_cpu_ids = 1;
	cpu_table[0].hartid = 0;
	cpu_table[0].state = CPU_ONLINE;
	cpu_table[0].idle_task = idle;
	cpu_table[0].current_task = idle;
}

struct task_struct *task_alloc(void)
{
	struct task_struct *task =
		kmalloc(sizeof(struct task_struct), ALLOC_NOWAIT);
	if (!task)
		return NULL;

	void *kstack = get_free_page(KSTACK_ORDER, ALLOC_NOWAIT);
	if (!kstack) {
		kfree(task);
		return NULL;
	}

	int32_t pid = alloc_pid();
	if (pid < 0) {
		free_page(kstack, KSTACK_ORDER);
		kfree(task);
		return NULL;
	}

	memset(task, 0, sizeof(struct task_struct));
	refcount_set(&task->lifecycle.refs, 1);
	task->ids.pid = (pid_t)pid;
	task->lifecycle.state = TASK_RUNNING;
	arch_task_init(task);
	task_set_kernel_stack(task, kstack);
	task->resources.mm = NULL;
	task->ids.tgid = task->ids.pid;
	task->ids.pgid = task->ids.pid;
	task->ids.sid = task->ids.pid;
	task->ids.group_leader = task;
	task->lifecycle.exit_signal = SIGCHLD;
	task->resources.uid = 0;
	task->resources.gid = 0;
	task->sigctx.sas.ss_flags = SS_DISABLE;
	sched_task_init(task);

	INIT_LIST_HEAD(&task->lifecycle.child_events);
	INIT_LIST_HEAD(&task->links.children);
	INIT_LIST_HEAD(&task->links.sibling);
	INIT_LIST_HEAD(&task->links.thread_group);
	INIT_LIST_HEAD(&task->links.thread_node);
	INIT_LIST_HEAD(&task->sched.run_list);
	wait_channel_init(&task->links.wait_child_queue);

	memset(kstack, 0, KSTACK_SIZE);

	return task;
}

int task_init_resources(struct task_struct *task)
{
	int ret;

	if (!task)
		return -EINVAL;

	ret = init_files(task);
	if (ret < 0)
		return ret;

	ret = init_fs(task);
	if (ret < 0)
		goto fail;

	ret = signals_init(task);
	if (ret < 0)
		goto fail;

	return 0;

fail:
	task_release_resources(task);
	return ret;
}

void task_release_resources(struct task_struct *task)
{
	if (!task)
		return;

	close_files(task);
	exit_fs(task);
	signals_release(task);
}

static void task_destroy(struct task_struct *task)
{
	BUG_ON(task->active_wait);
	task_child_event_discard_all(task);
	task_release_resources(task);

	free_pid(task->ids.pid);

	if (task_kernel_stack_safe(task)) {
		free_page(task_kernel_stack(task), KSTACK_ORDER);
		task_set_kernel_stack(task, NULL);
	}

	kfree(task);
}

void task_free(struct task_struct *task)
{
	if (!task)
		return;
	BUG_ON(task->active_wait);
	BUG_ON(task->lifecycle.published);
	BUG_ON(refcount_read(&task->lifecycle.refs) != 1);

	task_destroy(task);
}

void task_publish(struct task_struct *task)
{
	BUG_ON(refcount_read(&task->lifecycle.refs) <= 0);

	pid_attach_task(task->ids.pid, task);
}

void task_unpublish(struct task_struct *task)
{
	pid_detach_task(task->ids.pid, task);
}

bool task_try_get(struct task_struct *task)
{
	if (!task)
		return false;
	if (task == &idle_task)
		return true;

	return refcount_inc_not_zero(&task->lifecycle.refs);
}

void task_put(struct task_struct *task)
{
	if (!task || task == &idle_task)
		return;

	if (refcount_dec_and_test(&task->lifecycle.refs)) {
		BUG_ON(task->lifecycle.published);
		task_destroy(task);
	}
}

void task_init(void)
{
	pid_init();

	memset(&idle_task, 0, sizeof(struct task_struct));
	refcount_set(&idle_task.lifecycle.refs, 1);
	idle_task.ids.pid = 0;
	idle_task.lifecycle.state = TASK_RUNNING;
	arch_task_init(&idle_task);
	idle_task.resources.mm = NULL;
	idle_task.ids.tgid = idle_task.ids.pid;
	idle_task.ids.pgid = idle_task.ids.pid;
	idle_task.ids.sid = idle_task.ids.pid;
	idle_task.ids.group_leader = &idle_task;
	idle_task.lifecycle.exit_signal = SIGCHLD;
	idle_task.resources.uid = 0;
	idle_task.resources.gid = 0;
	idle_task.sigctx.sas.ss_flags = SS_DISABLE;
	sched_task_init(&idle_task);

	INIT_LIST_HEAD(&idle_task.lifecycle.child_events);
	INIT_LIST_HEAD(&idle_task.links.children);
	INIT_LIST_HEAD(&idle_task.links.sibling);
	INIT_LIST_HEAD(&idle_task.links.thread_group);
	INIT_LIST_HEAD(&idle_task.links.thread_node);
	INIT_LIST_HEAD(&idle_task.sched.run_list);
	wait_channel_init(&idle_task.links.wait_child_queue);
	BUG_ON(task_init_resources(&idle_task) < 0);
	task_publish(&idle_task);

	cpu_boot_init(&idle_task);
	set_current_task(&idle_task);

	pr_info("task: idle (PID 0) created\n");
}

struct task_struct *kernel_thread(void (*fn)(void *), void *arg)
{
	struct task_struct *parent = current_task();
	struct task_struct *task = task_alloc();

	if (!task)
		return NULL;
	if (task_init_resources(task) < 0) {
		task_free(task);
		return NULL;
	}

	task_setup_kthread(task, fn, arg);

	task_link_child(parent, task);

	task_publish(task);
	sched_enqueue(task);

	return task;
}

void set_init_task(struct task_struct *task)
{
	BUG_ON(task->ids.pid != 1);
	BUG_ON(init_task && init_task != task);

	init_task = task;
}

bool task_is_group_leader(const struct task_struct *task)
{
	return task && task->ids.group_leader == task;
}

bool task_group_has_other_threads(const struct task_struct *task)
{
	if (!task || !task->ids.group_leader)
		return false;

	return !list_empty(&task->ids.group_leader->links.thread_group);
}

struct task_struct *task_find_thread(pid_t tid)
{
	return pid_task_get(tid);
}

struct task_struct *task_find_group_leader(pid_t tgid)
{
	struct task_struct *task = pid_task_get(tgid);

	if (!task || !task_is_group_leader(task) || task->ids.tgid != tgid) {
		task_put(task);
		return NULL;
	}

	return task;
}

bool task_in_thread_group(const struct task_struct *task, pid_t tgid)
{
	return task && task->ids.tgid == tgid;
}

bool task_is_user_process(const struct task_struct *task)
{
	return task && task_is_group_leader(task) &&
	       atomic_read_acquire(&task->lifecycle.user_process) != 0;
}

void task_inherit_process_role(struct task_struct *child,
			       const struct task_struct *parent)
{
	BUG_ON(!child || !parent);
	atomic_set_release(
		&child->lifecycle.user_process,
		atomic_read_acquire(&parent->lifecycle.user_process));
}

void task_mark_user_process(struct task_struct *task)
{
	BUG_ON(!task);
	atomic_set_release(&task->lifecycle.user_process, 1);
}

static bool task_pgid_exists_visit(struct task_struct *task, void *arg)
{
	const struct task_pgid_query *query = arg;

	return task->ids.pgid == query->pgid;
}

static bool task_pgid_exists_locked(pid_t pgid)
{
	struct task_pgid_query query = {
		.pgid = pgid,
	};

	return pid_visit_published(task_pgid_exists_visit, &query);
}

static bool task_pgid_in_session_visit(struct task_struct *task, void *arg)
{
	const struct task_pgid_query *query = arg;

	return task->ids.pgid == query->pgid && task->ids.sid == query->sid;
}

static bool task_pgid_in_session_locked(pid_t pgid, pid_t sid)
{
	struct task_pgid_query query = {
		.pgid = pgid,
		.sid = sid,
	};

	return pid_visit_published(task_pgid_in_session_visit, &query);
}

static bool task_pgid_live_member_visit(struct task_struct *task, void *arg)
{
	const struct task_pgid_query *query = arg;

	return task != query->ignored && task->ids.pgid == query->pgid &&
	       task->ids.sid == query->sid && task_state(task) != TASK_ZOMBIE &&
	       task_state(task) != TASK_DEAD;
}

static bool
task_pgid_has_live_member_except_locked(pid_t pgid, pid_t sid,
					const struct task_struct *ignored)
{
	struct task_pgid_query query = {
		.pgid = pgid,
		.sid = sid,
		.ignored = ignored,
	};

	return pid_visit_published(task_pgid_live_member_visit, &query);
}

bool task_pgid_has_live_member_except(pid_t pgid, pid_t sid,
				      const struct task_struct *ignored)
{
	bool found;

	mutex_lock(&process_lock);
	found = task_pgid_has_live_member_except_locked(pgid, sid, ignored);
	mutex_unlock(&process_lock);
	return found;
}

int task_process_snapshot(const struct task_struct *task,
			  struct task_process_identity *identity)
{
	if (!task || !identity)
		return -EINVAL;

	mutex_lock(&process_lock);
	identity->pgid = task->ids.pgid;
	identity->sid = task->ids.sid;
	mutex_unlock(&process_lock);
	return 0;
}

int task_process_clone_identity(struct task_struct *child,
				const struct task_struct *parent)
{
	if (!child || !parent)
		return -EINVAL;
	if (child->lifecycle.published)
		return -EINVAL;

	mutex_lock(&process_lock);
	child->ids.pgid = parent->ids.pgid;
	child->ids.sid = parent->ids.sid;
	mutex_unlock(&process_lock);
	return 0;
}

static void task_set_pgid_all(struct task_struct *leader, pid_t pgid)
{
	struct task_struct *thread;

	leader = task_group_leader(leader);
	leader->ids.pgid = pgid;
	list_for_each_entry (thread, task_thread_group(leader),
			     links.thread_node)
		thread->ids.pgid = pgid;
}

static void task_set_sid_all(struct task_struct *leader, pid_t sid)
{
	struct task_struct *thread;

	leader = task_group_leader(leader);
	leader->ids.sid = sid;
	list_for_each_entry (thread, task_thread_group(leader),
			     links.thread_node)
		thread->ids.sid = sid;
}

int task_process_setsid(struct task_struct *task,
			struct task_process_identity *previous)
{
	struct task_struct *leader = task_group_leader_safe(task);
	pid_t sid;
	int ret = 0;

	if (!previous)
		return -EINVAL;
	if (!leader)
		return -ESRCH;

	mutex_lock(&process_lock);
	previous->pgid = leader->ids.pgid;
	previous->sid = leader->ids.sid;
	sid = task_pid(leader);
	if (task_pgid_exists_locked(sid)) {
		ret = -EPERM;
	} else {
		task_set_sid_all(leader, sid);
		task_set_pgid_all(leader, sid);
		ret = sid;
	}
	mutex_unlock(&process_lock);
	return ret;
}

int task_process_setpgid(struct task_struct *caller, pid_t pid, pid_t pgid,
			 struct task_process_identity *previous)
{
	struct task_struct *self = task_group_leader_safe(caller);
	struct task_struct *target;
	pid_t new_pgid;
	bool put_target = false;
	int ret = 0;

	if (!previous)
		return -EINVAL;
	if (!self)
		return -ESRCH;

	mutex_lock(&process_lock);
	if (pid == 0) {
		target = self;
	} else {
		target = task_find_thread(pid);
		put_target = true;
	}
	if (!target || !task_is_group_leader(target) ||
	    task_tgid(target) != task_pid(target)) {
		ret = -ESRCH;
		goto out;
	}
	if (target != self && target->links.parent != self) {
		ret = -EPERM;
		goto out;
	}
	if (target != self && target->ids.sid != self->ids.sid) {
		ret = -EPERM;
		goto out;
	}
	if (target->ids.sid == task_pid(target)) {
		ret = -EPERM;
		goto out;
	}

	new_pgid = pgid == 0 ? task_pid(target) : pgid;
	if (new_pgid != task_pid(target) &&
	    !task_pgid_in_session_locked(new_pgid, target->ids.sid)) {
		ret = -EPERM;
		goto out;
	}

	previous->pgid = target->ids.pgid;
	previous->sid = target->ids.sid;
	task_set_pgid_all(target, new_pgid);
out:
	mutex_unlock(&process_lock);
	if (put_target && target)
		task_put(target);
	return ret;
}

struct task_struct *task_parent(struct task_struct *task)
{
	struct task_struct *parent;
	irq_flags_t flags;

	if (!task)
		return NULL;

	task_child_lock(&flags);
	parent = task->links.parent;
	task_child_unlock(flags);
	return parent;
}

void task_link_child(struct task_struct *parent, struct task_struct *child)
{
	irq_flags_t flags;

	if (!parent || !child)
		return;

	task_child_lock(&flags);
	child->links.parent = parent;
	list_add_tail(&child->links.sibling, &parent->links.children);
	task_child_unlock(flags);
}

void task_unlink_child(struct task_struct *task)
{
	irq_flags_t flags;

	if (!task)
		return;

	task_child_lock(&flags);
	if (!list_empty(&task->links.sibling)) {
		list_del_init(&task->links.sibling);
		task->links.parent = NULL;
	}
	task_child_unlock(flags);
}

uint32_t task_child_count(const struct task_struct *parent)
{
	struct task_struct *child;
	irq_flags_t flags;
	uint32_t count = 0;

	if (!parent)
		return 0;

	task_child_lock(&flags);
	list_for_each_entry (child, &parent->links.children, links.sibling)
		count++;
	task_child_unlock(flags);
	return count;
}

void task_child_publish_stop(struct task_struct *task, int sig)
{
	struct task_child_event_record *event;
	struct task_child_notification notification;
	irq_flags_t flags;

	if (!task_is_group_leader(task))
		return;

	event = task_child_event_alloc(TASK_CHILD_EVENT_STOP,
				       child_stop_status(sig));
	task_child_lock(&flags);
	task_child_event_publish_locked(task, event, CLD_STOPPED, sig,
					&notification);
	task_child_unlock(flags);
	task_child_event_send_notification(&notification);
}

void task_child_publish_continue(struct task_struct *task)
{
	struct task_child_event_record *event;
	struct task_child_notification notification;
	irq_flags_t flags;

	if (!task_is_group_leader(task))
		return;

	event = task_child_event_alloc(TASK_CHILD_EVENT_CONTINUE, 0xffff);
	task_child_lock(&flags);
	task_child_event_publish_locked(task, event, CLD_CONTINUED, SIGCONT,
					&notification);
	task_child_unlock(flags);
	task_child_event_send_notification(&notification);
}

void task_child_publish_exit(struct task_struct *task, int status)
{
	struct task_child_event_record *event;
	struct task_child_notification notification;
	struct task_child_wakeup reparent_wakeup = {0};
	irq_flags_t flags;

	BUG_ON(!task_is_group_leader(task));
	event = task_child_event_alloc(TASK_CHILD_EVENT_EXIT, status);
	task_child_lock(&flags);
	task_reparent_children_locked(task, &reparent_wakeup);
	task_set_state(task, TASK_ZOMBIE);
	task_child_event_publish_locked(task, event, child_exit_si_code(status),
					child_exit_si_status(status),
					&notification);
	task_child_unlock(flags);
	task_child_wake_parent(&reparent_wakeup);
	task_child_event_send_notification(&notification);
}

enum task_child_wait_state
task_child_event_claim_next(struct task_struct *parent, pid_t pid,
			    uint32_t event_mask,
			    struct task_child_event_claim *claim)
{
	struct task_child_event_record *event;
	struct task_struct *child;
	enum task_child_wait_state state;
	irq_flags_t flags;

	memset(claim, 0, sizeof(*claim));
	task_child_lock(&flags);
	state = task_child_event_observe_locked(parent, pid, event_mask, &child,
						&event);
	if (state == TASK_CHILD_WAIT_EVENT) {
		event->claimer = parent;
		claim->parent = parent;
		claim->child = child;
		claim->pid = task_pid(child);
		claim->sequence = event->sequence;
		claim->type = event->type;
		claim->status = event->status;
	}
	task_child_unlock(flags);
	return state;
}

int task_child_event_watch(struct wait_session *session,
			   struct task_struct *parent, pid_t pid,
			   uint32_t event_mask)
{
	enum task_child_wait_state state;
	irq_flags_t flags;
	int ret;

	task_child_lock(&flags);
	state = task_child_event_observe_locked(parent, pid, event_mask, NULL,
						NULL);
	if (state != TASK_CHILD_WAIT_NO_EVENT) {
		ret = 1;
	} else {
		ret = wait_session_watch(session,
					 &parent->links.wait_child_queue);
		if (ret == 0 && task_child_event_observe_locked(
					parent, pid, event_mask, NULL, NULL) !=
					TASK_CHILD_WAIT_NO_EVENT)
			ret = 1;
	}
	task_child_unlock(flags);
	return ret;
}

bool task_child_event_commit(const struct task_child_event_claim *claim)
{
	struct task_child_event_record *event;
	irq_flags_t flags;
	bool committed = false;

	if (!claim->parent || !claim->child)
		return false;

	task_child_lock(&flags);
	event = task_child_event_find_sequence_locked(claim->child,
						      claim->sequence);
	if (event && event->claimer == claim->parent) {
		list_del_init(&event->node);
		committed = true;
	}
	task_child_unlock(flags);
	if (committed)
		kfree(event);
	return committed;
}

void task_child_event_abort(const struct task_child_event_claim *claim)
{
	struct task_child_event_record *event;
	struct task_struct *parent = NULL;
	bool parent_ref = false;
	irq_flags_t flags;

	if (!claim->parent || !claim->child)
		return;

	task_child_lock(&flags);
	event = task_child_event_find_sequence_locked(claim->child,
						      claim->sequence);
	if (event && event->claimer == claim->parent) {
		event->claimer = NULL;
		if (task_try_get(claim->parent)) {
			parent = claim->parent;
			parent_ref = parent != &idle_task;
		}
	}
	task_child_unlock(flags);
	if (parent) {
		wait_channel_wake_one(&parent->links.wait_child_queue);
		if (parent_ref)
			task_put(parent);
	}
}

void task_child_release_zombie(struct task_struct *task)
{
	LIST_HEAD(discarded);
	struct task_struct *parent;
	bool parent_ref = false;
	bool wake_parent = false;
	irq_flags_t flags;

	task_child_lock(&flags);
	BUG_ON(task_state(task) != TASK_ZOMBIE);
	BUG_ON(!list_empty(&task->links.children));
	BUG_ON(task_is_group_leader(task) &&
	       !list_empty(&task->links.thread_group));
	if (!list_empty(&task->links.sibling))
		list_del_init(&task->links.sibling);
	parent = task->links.parent;
	task->links.parent = NULL;
	task_set_state(task, TASK_DEAD);
	task_child_event_discard_all_locked(task, &discarded);
	if (parent && task_try_get(parent)) {
		parent_ref = parent != &idle_task;
		wake_parent = true;
	}
	task_child_unlock(flags);
	if (wake_parent) {
		wait_channel_wake_all(&parent->links.wait_child_queue);
		if (parent_ref)
			task_put(parent);
	}
	task_child_event_free_list(&discarded);
}

#ifdef KERNEL_SELFTEST
pid_t task_test_pgid(const struct task_struct *task)
{
	struct task_process_identity identity;

	BUG_ON(task_process_snapshot(task, &identity) < 0);
	return identity.pgid;
}

pid_t task_test_sid(const struct task_struct *task)
{
	struct task_process_identity identity;

	BUG_ON(task_process_snapshot(task, &identity) < 0);
	return identity.sid;
}

void task_test_set_process_identity(struct task_struct *task, pid_t pgid,
				    pid_t sid)
{
	BUG_ON(!task);

	mutex_lock(&process_lock);
	task->ids.pgid = pgid;
	task->ids.sid = sid;
	mutex_unlock(&process_lock);
}

void task_test_mark_user_process(struct task_struct *task)
{
	task_mark_user_process(task);
}

void task_test_inherit_process_role(struct task_struct *child,
				    const struct task_struct *parent)
{
	task_inherit_process_role(child, parent);
}
#endif

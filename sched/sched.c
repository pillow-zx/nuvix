/* sched/sched.c - generic scheduler mechanism */

#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/rseq.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>
#include <nuvix/trap.h>

#include "internal.h"

static struct runqueue runqueues[NR_CPUS];
struct retired_queue {
	spinlock_t lock;
	struct list_head tasks;
};

static struct retired_queue retired_queues[NR_CPUS];
/* Exiting tasks whose kernel stack is still in use until the next context
 * switch completes on this CPU.  The switch-core tail, the exit path, and
 * the tick drain this list into the retired queue, so the reaper only ever
 * pops tasks whose stack is provably abandoned. */
static struct list_head retired_pending[NR_CPUS];
static const struct sched_ops *policy = &rr_ops;

static void sched_switch_current(void)
{
	bool restore_irqoff = irqs_disabled();

	/*
	 * schedule() requires IRQs enabled, but callers retain their entry
	 * state.
	 */
	if (restore_irqoff)
		local_irq_enable();
	schedule();
	if (restore_irqoff)
		local_irq_disable();
}

void task_switch(struct task_struct *prev, struct task_struct *next)
{
	BUG_ON(!irqs_disabled());
	BUG_ON(in_irq());
	BUG_ON(!preemptible());
	BUG_ON(spinlock_held());

	set_current_task(next);
	arch_task_switch(prev, next);
}

static void sched_handoff(struct task_struct *prev, struct task_struct *next)
{
	rseq_sched_switch(prev);
	task_switch(prev, next);
}

static struct runqueue *sched_rq_for_cpu(struct cpu *cpu)
{
	/* The CPU pointer is the current hart's tp or a task's assigned CPU;
	 * both must always be valid. */
	BUG_ON(!cpu);
	return &runqueues[cpu->id];
}

static struct runqueue *sched_rq_for_task(struct task_struct *task)
{
	struct cpu *cpu = task && task->cpu ? task->cpu : current_cpu();

	return sched_rq_for_cpu(cpu);
}

static void sched_enqueue_locked(struct runqueue *rq, struct task_struct *task,
				 enum sched_enqueue_reason reason)
{
	BUG_ON(!rq || !task || task->on_rq);
	/* Affinity invariant: a task may only sit on a runqueue whose CPU is
	 * in its allowed mask. CPU assignment is scheduler-owner state. */
	BUG_ON(!(task->allowed_cpus & (1ULL << rq->cpu_id)));
	/* Direct slot indexing: runqueues are indexed 0..NR_CPUS-1 and the
	 * slot always exists; cpu_by_id() would truncate at nr_cpu_ids. */
	task->cpu = &cpu_table[(uint32_t)(rq - runqueues)];
	policy->enqueue(rq, task, reason);
	task->on_rq = true;
	rq->nr_running++;
}

static void sched_dequeue_locked(struct runqueue *rq, struct task_struct *task)
{
	BUG_ON(!rq || !task || !task->on_rq);
	policy->dequeue(rq, task);
	task->on_rq = false;
	BUG_ON(rq->nr_running == 0);
	rq->nr_running--;
}

static struct task_struct *sched_pick_locked(struct runqueue *rq)
{
	struct task_struct *next = policy->pick_next(rq);

	if (!next)
		return rq->idle;
	sched_dequeue_locked(rq, next);
	return next;
}

static void sched_switch_locked(struct runqueue *rq, struct task_struct *prev,
				struct task_struct *next)
{
	if (prev && prev != rq->idle)
		prev->on_cpu = false;
	if (next) {
		next->cpu = &cpu_table[(uint32_t)(rq - runqueues)];
		next->on_cpu = true;
		next->run_state = TASK_RUNNING;
	}
	rq->current = next;
}

void sched_task_init(struct task_struct *task)
{
	INIT_LIST_HEAD(&task->sched.run_node);
	atomic_set(&task->sched.need_resched, 0);
}

void sched_init(void)
{
	policy->init();
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		struct runqueue *rq = &runqueues[id];

		spin_lock_init(&rq->lock, LOCK_RANK_RUNQUEUE);
		rq->cpu_id = id;
		INIT_LIST_HEAD(&rq->runnable);
		rq->nr_running = 0;
		/* Direct slot indexing like cpu_boot_init(): every enumerated slot
		 * is prepared. Offline CPUs keep a NULL current until brought up. */
		rq->idle = cpu_table[id].idle_task;
		rq->current = cpu_table[id].current_task;
		spin_lock_init(&retired_queues[id].lock, LOCK_RANK_RETIRED);
		INIT_LIST_HEAD(&retired_queues[id].tasks);
		INIT_LIST_HEAD(&retired_pending[id]);
	}
}

void sched_enqueue_new(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t flags;

	if (!task || task->lifecycle != TASK_LIVE || task->on_rq ||
	    task->on_cpu || task_is_exiting(task))
		return;
	rq = sched_rq_for_task(task);
	if (!(task->allowed_cpus & (1ULL << rq->cpu_id)))
		return;
	spin_lock_irqsave(&rq->lock, &flags);
	task->run_state = TASK_RUNNABLE;
	sched_enqueue_locked(rq, task, SCHED_ENQUEUE_NEW);
	spin_unlock_irqrestore(&rq->lock, flags);
}

void sched_dequeue(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t flags;

	if (!task || !task->on_rq)
		return;
	rq = sched_rq_for_task(task);
	spin_lock_irqsave(&rq->lock, &flags);
	if (task->on_rq)
		sched_dequeue_locked(rq, task);
	spin_unlock_irqrestore(&rq->lock, flags);
}

int sched_block_current(struct task_wait *wait)
{
	struct task_struct *task = current_task();
	struct runqueue *rq;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	bool block = false;

	if (!wait || !task || wait != &task->wait || task_is_idle(task))
		return -EINVAL;
	rq = sched_rq_for_cpu(current_cpu());
	spin_lock_irqsave(&wait->lock, &wait_flags);
	if (wait->status == WAIT_ACTIVE && wait->reason == TASK_WAKE_NONE &&
	    task->lifecycle == TASK_LIVE && task->run_state == TASK_RUNNING) {
		spin_lock_irqsave(&rq->lock, &rq_flags);
		task->run_state = TASK_BLOCKED;
		block = true;
		spin_unlock_irqrestore(&rq->lock, rq_flags);
	}
	spin_unlock_irqrestore(&wait->lock, wait_flags);
	if (block)
		sched_switch_current();
	return block ? 1 : 0;
}

bool sched_wake(struct task_struct *task, uint64_t generation)
{
	struct runqueue *rq;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	bool woke = false;

	if (!task || task_is_idle(task))
		return false;
	rq = sched_rq_for_task(task);
	/* The affinity check guards the enqueue target, not the waker's CPU:
	 * on SMP a remote wake would otherwise pass a local-only check. */
	if (!(task->allowed_cpus & (1ULL << rq->cpu_id)))
		return false;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	if (task->wait.status == WAIT_ACTIVE &&
	    task->wait.generation == generation &&
	    task->lifecycle == TASK_LIVE && task->run_state == TASK_BLOCKED) {
		spin_lock_irqsave(&rq->lock, &rq_flags);
		task->run_state = TASK_RUNNABLE;
		if (!task->on_rq)
			sched_enqueue_locked(rq, task, SCHED_ENQUEUE_WAKE);
		woke = true;
		spin_unlock_irqrestore(&rq->lock, rq_flags);
	}
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	if (woke && task->cpu != current_cpu())
		arch_sched_remote_wake(task->cpu);
	return woke;
}

int sched_set_affinity(struct task_struct *task, uint64_t mask)
{
	uint64_t online = cpu_schedulable_mask();
	struct runqueue *rq;
	irq_flags_t flags;
	int ret = 0;

	if (!task || !mask || !(mask & online))
		return -EINVAL;
	/* The on_rq/on_cpu check must race with no enqueue: hold the task's
	 * runqueue lock around re-check and store (no migration on SMP
	 * until a migration path exists, so the owning rq is fixed). */
	rq = sched_rq_for_task(task);
	spin_lock_irqsave(&rq->lock, &flags);
	if (task->on_rq || task->on_cpu)
		ret = -EBUSY;
	else
		task->allowed_cpus = mask & online;
	spin_unlock_irqrestore(&rq->lock, flags);
	return ret;
}

uint64_t sched_get_affinity(const struct task_struct *task)
{
	return task ? task->allowed_cpus & cpu_schedulable_mask() : 0;
}

bool sched_wake_external(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t flags;
	bool woke = false;

	if (!task || task_is_idle(task) || task->lifecycle != TASK_LIVE)
		return false;
	rq = sched_rq_for_task(task);
	spin_lock_irqsave(&rq->lock, &flags);
	if (!task->on_rq && !task->on_cpu) {
		task->run_state = TASK_RUNNABLE;
		sched_enqueue_locked(rq, task, SCHED_ENQUEUE_WAKE);
		woke = true;
	}
	spin_unlock_irqrestore(&rq->lock, flags);
	return woke;
}

bool sched_retired_pop(struct task_struct **task)
{
	if (!task)
		return false;
	*task = NULL;
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		struct retired_queue *queue = &retired_queues[id];
		irq_flags_t flags;

		spin_lock_irqsave(&queue->lock, &flags);
		if (!list_empty(&queue->tasks)) {
			*task = list_first_entry(&queue->tasks,
						 struct task_struct,
						 retired_node);
			list_del_init(&(*task)->retired_node);
			spin_unlock_irqrestore(&queue->lock, flags);
			return true;
		}
		spin_unlock_irqrestore(&queue->lock, flags);
	}
	return false;
}

static void sched_retired_drain(void)
{
	struct retired_queue *retired = &retired_queues[current_cpu()->id];
	struct list_head *pending = &retired_pending[current_cpu()->id];
	struct list_head *pos;
	struct list_head *n;
	irq_flags_t flags;

	if (list_empty(pending))
		return;
	spin_lock_irqsave(&retired->lock, &flags);
	list_for_each_safe (pos, n, pending) {
		struct task_struct *task = list_entry(pos, struct task_struct,
						      retired_node);

		/* Only reached after the task's own handoff completed, so
		 * its stack is abandoned; on_cpu was cleared under the
		 * runqueue lock before that switch. */
		BUG_ON(task->on_cpu);
		list_move_tail(pos, &retired->tasks);
	}
	spin_unlock_irqrestore(&retired->lock, flags);
}

bool sched_stop(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t flags;
	bool stopped = false;

	if (!task || task_is_idle(task) || task->lifecycle != TASK_LIVE)
		return false;
	rq = sched_rq_for_task(task);
	spin_lock_irqsave(&rq->lock, &flags);
	if (task->run_state != TASK_STOPPED) {
		if (task->on_rq)
			sched_dequeue_locked(rq, task);
		task->run_state = TASK_STOPPED;
		stopped = true;
	}
	spin_unlock_irqrestore(&rq->lock, flags);
	return stopped;
}

bool sched_resume(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t flags;
	bool resumed = false;

	if (!task || task_is_idle(task) || task->lifecycle != TASK_LIVE)
		return false;
	rq = sched_rq_for_task(task);
	spin_lock_irqsave(&rq->lock, &flags);
	if (task->run_state == TASK_STOPPED) {
		task->run_state = TASK_RUNNABLE;
		if (!task->on_rq)
			sched_enqueue_locked(rq, task, SCHED_ENQUEUE_WAKE);
		resumed = true;
	}
	spin_unlock_irqrestore(&rq->lock, flags);
	return resumed;
}

bool sched_has_runnable(void)
{
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	return rq->nr_running != 0;
}

/*
 * The one switch core behind both entries: enqueue the preempted task,
 * pick, switch, hand off. IRQ state is restored to whatever the caller
 * entered with, so the trap-return path may call in with IRQs already
 * disabled (both entries share one scheduler core).
 */
static void sched_switch_core(void)
{
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	struct task_struct *prev = current_task();
	struct task_struct *next;
	irq_flags_t flags;

	flags = local_irq_save();
	spin_lock(&rq->lock);
	task_set_need_resched(prev, 0);
	if (prev != rq->idle && prev->lifecycle == TASK_LIVE &&
	    prev->run_state == TASK_RUNNING && !prev->on_rq)
		sched_enqueue_locked(rq, prev, SCHED_ENQUEUE_PREEMPT);
	next = sched_pick_locked(rq);
	if (!next)
		next = rq->idle;
	sched_switch_locked(rq, prev, next);
	spin_unlock(&rq->lock);
	if (next == prev) {
		local_irq_restore(flags);
		return;
	}
	sched_handoff(prev, next);
	/* After an exit handoff this runs in the next task's context and
	 * publishes the exiting tasks whose stacks were abandoned by it. */
	sched_retired_drain();
	local_irq_restore(flags);
}

void schedule(void)
{
	BUG_ON(in_irq());
	BUG_ON(!current_task());
	BUG_ON(!preemptible());
	BUG_ON(spinlock_held());
	BUG_ON(irqs_disabled());

	sched_switch_core();
}

void schedule_irqoff(void)
{
	/* The trap-return path owns IRQ restoration after this handoff. */
	BUG_ON(!irqs_disabled());
	BUG_ON(in_irq());
	BUG_ON(!current_task());
	BUG_ON(!preemptible());
	BUG_ON(spinlock_held());

	sched_switch_core();
}

__noreturn
void sched_exit_current(void)
{
	struct runqueue *rq;
	struct task_struct *prev;
	struct task_struct *next;
	irq_flags_t flags;

	BUG_ON(!current_task() || task_is_idle(current_task()));
	BUG_ON(current_task()->lifecycle != TASK_DEAD);

	local_irq_disable();
	rq = sched_rq_for_cpu(current_cpu());
	prev = current_task();
	spin_lock_irqsave(&rq->lock, &flags);
	BUG_ON(rq->current != prev || prev->on_rq || prev->on_cpu == false);
	prev->run_state = TASK_STOPPED;
	prev->on_cpu = false;
	next = sched_pick_locked(rq);
	if (!next)
		next = rq->idle;
	sched_switch_locked(rq, prev, next);
	spin_unlock_irqrestore(&rq->lock, flags);
	BUG_ON(next == prev);
	/* Publish tasks abandoned by earlier switches before this one. */
	sched_retired_drain();
	BUG_ON(prev->on_cpu);
	/* The retired publication is deferred until after the handoff
	 * completes (the switch-core tail or the tick drains this list),
	 * so the reaper can never free a stack still in use. */
	list_add_tail(&prev->retired_node, &retired_pending[current_cpu()->id]);
	sched_handoff(prev, next);
	panic("sched: exited task resumed");
	unreachable();
}

void sched_request(void)
{
	struct task_struct *task = current_task();

	if (task && !task_is_idle(task))
		task_set_need_resched(task, 1);
}

void sched_tick(void)
{
	struct task_struct *task = current_task();
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	irq_flags_t flags;
	bool expire;

	/* Bounded fallback for publishing retired tasks whose exit handoff
	 * switched to a task that never reaches the switch-core tail. */
	sched_retired_drain();
	if (!task || task_is_idle(task))
		return;
	if (task_trap_frome_user(task))
		task->cputime.utime_ticks++;
	else
		task->cputime.stime_ticks++;
	spin_lock_irqsave(&rq->lock, &flags);
	expire = policy->tick(rq, task);
	spin_unlock_irqrestore(&rq->lock, flags);
	if (expire)
		sched_request();
}

void sched_yield(void)
{
	struct task_struct *task = current_task();

	if (!task || task_is_idle(task))
		return;
	sched_request();
	sched_switch_current();
}

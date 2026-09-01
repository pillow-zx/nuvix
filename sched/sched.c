/* sched/sched.c - generic scheduler mechanism */

#include <nuvix/bitops.h>
#include <nuvix/errno.h>
#include <nuvix/ipi.h>
#include <nuvix/mm.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/rseq.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/trap.h>
#include <arch/pgtable.h>

#include "internal.h"

/*
 * Placement ownership: task->cpu, on_rq, and the affinity masks are written
 * only while holding the owning Task's wait lock, with the runqueue lock
 * acquired after it (queue membership changes).  on_cpu is the switch-out
 * witness: it is published for the picked Task under the runqueue lock
 * alone (the switch-in exception in sched_switch_locked) and cleared only
 * after the architecture handoff completed, under the Task's wait lock in
 * Switch Completion.  While it is set, the Task's context may still be
 * live or being saved, so no path may enqueue it. Runqueue locks are never
 * held two at a time anywhere in the scheduler; every placement path takes
 * exactly one.
 */
static struct runqueue runqueues[NR_CPUS];

struct retired_queue {
	spinlock_t lock;
	struct list_head tasks;
};

static struct retired_queue retired_queues[NR_CPUS];
static const struct sched_ops *policy = &rr_ops;

static void sched_switch_complete(struct task_struct *last);

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

struct task_struct *task_switch(struct task_struct *prev,
				struct task_struct *next,
				uintptr_t next_pgroot)
{
	BUG_ON(!irqs_disabled());
	BUG_ON(in_irq());
	BUG_ON(!preemptible());
	BUG_ON(spinlock_held());

	set_current_task(next);
	return arch_task_switch(prev, next, next_pgroot);
}

static struct task_struct *sched_handoff(struct task_struct *prev,
					 struct task_struct *next,
					 uintptr_t next_pgroot)
{
	rseq_sched_switch(prev);
	return task_switch(prev, next, next_pgroot);
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

/*
 * Least-loaded selection hint: pick the schedulable CPU inside the allowed
 * mask with the fewest queued Tasks, breaking ties toward the lower logical
 * ID.  The load read is unsynchronized by design -- the runqueue lock guards
 * queue membership, and this is only a placement hint, not a guarantee.
 */
static struct cpu *sched_select_cpu(const cpumask_t *mask)
{
	uint32_t best = NR_CPUS;
	uint32_t best_load = UINT32_MAX;
	uint32_t id;

	if (!mask)
		return NULL;
	for (id = 0; id < nr_cpu_ids; id++) {
		uint32_t load;

		if (!cpumask_test_cpu(mask, id) || !cpu_is_schedulable(id))
			continue;
		load = runqueues[id].nr_running;
		if (load < best_load) {
			best_load = load;
			best = id;
		}
	}
	return best < NR_CPUS ? &cpu_table[best] : NULL;
}

/*
 * Migration-Pending: a running Task whose current CPU is outside its
 * Effective Affinity.  A named predicate, not a stored flag: the state is
 * tolerated until the Task's own switch-out, where the re-home runs under
 * the Task's wait lock.  With a non-preemptible kernel the window is
 * bounded by the Task's next user return, block, or yield.
 */
static bool sched_task_migration_pending(const struct task_struct *task)
{
	return task->on_cpu && task->cpu &&
	       !cpumask_test_cpu(&task->effective_affinity, task->cpu->id);
}

static struct runqueue *sched_rq_for_task_locked(struct task_struct *task)
{
	struct cpu *cpu = task->cpu;

	if (!cpu || !cpu_is_schedulable(cpu->id) ||
	    !cpumask_test_cpu(&task->effective_affinity, cpu->id))
		cpu = sched_select_cpu(&task->effective_affinity);
	BUG_ON(!cpu);
	return sched_rq_for_cpu(cpu);
}

static void sched_notify_remote(uint32_t cpu_id)
{
	if (cpu_id != current_cpu()->id)
		BUG_ON(ipi_send(cpu_id, IPI_RESCHEDULE) != 0);
}

static void sched_enqueue_locked(struct runqueue *rq, struct task_struct *task,
				 enum sched_enqueue_reason reason)
{
	BUG_ON(!rq || !task || task->on_rq);
	BUG_ON(!cpu_is_schedulable(rq->cpu_id));
	/* Affinity invariant: a task may only sit on a runqueue whose CPU is
	 * in its effective affinity. */
	BUG_ON(!cpumask_test_cpu(&task->effective_affinity, rq->cpu_id));
	/* Switch Completion must publish the Task non-active before any
	 * runqueue can dispatch its saved context. */
	BUG_ON(task->on_cpu);
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
	/* Dequeueing the picked Task here is part of the switch-in
	 * publication exception (see sched_switch_locked): the pick holds
	 * only the runqueue lock. */
	sched_dequeue_locked(rq, next);
	return next;
}

/*
 * Switch-in publication, the single wait-lock-free placement write set:
 * the picked Task's dequeue at the pick and this publication of
 * on_cpu=true and TASK_RUNNING happen under the runqueue lock alone,
 * without the Task's wait lock.  The rank order forbids acquiring the
 * wait lock inside the runqueue lock, and the picked Task is dequeued at
 * that moment, so the dequeued-but-not-running window is visible to no
 * observer: pick and publication share one runqueue-lock critical
 * section, wake paths re-validate on_rq/on_cpu under that same lock, and
 * every other placement decision is serialized by the wait lock.  The
 * pick does not rewrite task->cpu: an enqueued Task carries the CPU of
 * the runqueue it sits on, and idle Tasks received their CPU at boot.
 * prev->on_cpu is deliberately not cleared here: it stays set until the
 * architecture handoff completed and Switch Completion publishes the
 * switch-out under the Task's wait lock, so the context-save window is
 * covered by the same witness.
 */
static void sched_switch_locked(struct runqueue *rq, struct task_struct *next)
{
	if (next) {
		if (next != rq->idle) {
			BUG_ON(!cpu_is_schedulable(rq->cpu_id));
			next->on_cpu = true;
		}
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

		spin_lock_init(&rq->lock, LOCK_RANK_RUNQUEUE,
				LOCK_IRQ_HARDIRQ_REACHABLE);
		rq->cpu_id = id;
		INIT_LIST_HEAD(&rq->runnable);
		rq->nr_running = 0;
		/* Direct slot indexing like cpu_boot_init(): every enumerated slot
		 * is prepared. Offline CPUs keep a NULL current until brought up. */
		rq->idle = cpu_table[id].idle_task;
		rq->current = cpu_table[id].current_task;
		rq->active_mm = NULL;
		rq->handoff.outgoing = NULL;
		rq->handoff.incoming = NULL;
		rq->handoff.incoming_mm = NULL;
		rq->handoff.installed_pgroot = 0;
		rq->handoff.terminal = false;
		rq->handoff.pending = false;
		spin_lock_init(&retired_queues[id].lock, LOCK_RANK_RETIRED,
				LOCK_IRQ_HARDIRQ_REACHABLE);
		INIT_LIST_HEAD(&retired_queues[id].tasks);
	}
}

void sched_enqueue_new(struct task_struct *task)
{
	struct runqueue *rq;
	struct cpu *cpu;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	bool enqueued = false;

	if (!task)
		return;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	if (task->lifecycle == TASK_LIVE && !task->on_rq && !task->on_cpu &&
	    !task_is_exiting(task)) {
		/* New-Task placement: least-loaded CPU inside the Effective
		 * Affinity.  Wake and resume, by contrast, keep a Task's
		 * current CPU while it stays legal. */
		cpu = sched_select_cpu(&task->effective_affinity);
		BUG_ON(!cpu);
		rq = sched_rq_for_cpu(cpu);
		spin_lock_irqsave(&rq->lock, &rq_flags);
		task->run_state = TASK_RUNNABLE;
		sched_enqueue_locked(rq, task, SCHED_ENQUEUE_NEW);
		spin_unlock_irqrestore(&rq->lock, rq_flags);
		enqueued = true;
	}
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	if (enqueued)
		sched_notify_remote(rq->cpu_id);
}

void sched_dequeue(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;

	if (!task)
		return;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	/* A queued Task is dequeued from the runqueue it actually sits on
	 * -- task->cpu's -- never from a reselected one. */
	rq = sched_rq_for_task(task);
	spin_lock_irqsave(&rq->lock, &rq_flags);
	if (task->on_rq)
		sched_dequeue_locked(rq, task);
	spin_unlock_irqrestore(&rq->lock, rq_flags);
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
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
	/* Block only if nothing has already woken us: an event wake clears the
	 * event_fired hint, and a signal must not block once its predicate is
	 * true.  This mirrors wait_block(), preserving the wake-then-block race
	 * guard. */
	if (wait->status == WAIT_ACTIVE && !wait->event_fired &&
	    !(wait->policy == TASK_WAIT_INTERRUPTIBLE &&
	      sig_wait_ready(task, wait)) &&
	    !(wait->policy == TASK_WAIT_KILLABLE &&
	      sig_fatal_pending(task)) &&
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
	uint32_t cpu_id = UINT32_MAX;
	bool woke = false;

	if (!task || task_is_idle(task))
		return false;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	if (task->wait.status == WAIT_ACTIVE &&
	    task->wait.generation == generation &&
	    task->lifecycle == TASK_LIVE && task->run_state == TASK_BLOCKED) {
		/* A Task whose context is still being saved remains unqueued;
		 * Switch Completion places it after publishing on_cpu=false. */
		if (task->on_cpu) {
			task->run_state = TASK_RUNNABLE;
			woke = true;
		} else {
			rq = sched_rq_for_task_locked(task);
			BUG_ON(!cpumask_test_cpu(&task->effective_affinity,
						 rq->cpu_id));
			spin_lock_irqsave(&rq->lock, &rq_flags);
			task->run_state = TASK_RUNNABLE;
			if (!task->on_rq)
				sched_enqueue_locked(rq, task,
						     SCHED_ENQUEUE_WAKE);
			woke = true;
			cpu_id = rq->cpu_id;
			spin_unlock_irqrestore(&rq->lock, rq_flags);
		}
	}
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	if (cpu_id != UINT32_MAX)
		sched_notify_remote(cpu_id);
	return woke;
}

static void sched_possible_mask(cpumask_t *mask)
{
	cpumask_zero(mask);
	for (uint32_t id = 0; id < nr_cpu_ids; id++)
		cpumask_set_cpu(mask, id);
}

static void sched_policy_mask(cpumask_t *mask)
{
	cpumask_zero(mask);
	for (uint32_t id = 0; id < nr_cpu_ids; id++)
		if (cpu_is_schedulable(id))
			cpumask_set_cpu(mask, id);
}

int sched_set_affinity(struct task_struct *task, const cpumask_t *requested)
{
	cpumask_t possible;
	cpumask_t normalized;
	cpumask_t policy_mask;
	cpumask_t effective;
	struct runqueue *home_rq;
	struct runqueue *target_rq;
	struct cpu *target;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	uint32_t notify_id = UINT32_MAX;

	if (!task || !requested)
		return -EINVAL;
	sched_possible_mask(&possible);
	cpumask_and(&normalized, requested, &possible);
	if (cpumask_empty(&normalized))
		return -EINVAL;
	sched_policy_mask(&policy_mask);
	cpumask_and(&effective, &normalized, &policy_mask);
	if (cpumask_empty(&effective))
		return -EINVAL;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	/* The placement decision is serialized by the Task wait lock; the
	 * run state is classified under the home runqueue lock, which the
	 * switch-in exception holds across publication. */
	cpumask_copy(&task->requested_affinity, &normalized);
	cpumask_copy(&task->effective_affinity, &effective);
	home_rq = sched_rq_for_task(task);
	spin_lock_irqsave(&home_rq->lock, &rq_flags);
	if (task->on_cpu) {
		/* Running.  on_cpu also covers the switch-out context-save
		 * window, so this branch is taken for any Task that may
		 * still be dispatched.  Migration-Pending is the predicate:
		 * nudge the CPU and let the Task re-home at its own
		 * switch-out; no busy outcome exists here. */
		if (sched_task_migration_pending(task)) {
			task_set_need_resched(task, 1);
			notify_id = task->cpu->id;
		}
	} else if (task->on_rq) {
		target = sched_select_cpu(&task->effective_affinity);
		BUG_ON(!target);
		if (target->id != home_rq->cpu_id) {
			sched_dequeue_locked(home_rq, task);
			spin_unlock_irqrestore(&home_rq->lock, rq_flags);
			/* Release the source before acquiring the target:
			 * runqueue locks are never held two at a time, and
			 * the off-queue Task is invisible to pickers (they
			 * hold the source lock) and to every other
			 * placement agent (they need the Task wait lock). */
			target_rq = sched_rq_for_cpu(target);
			spin_lock_irqsave(&target_rq->lock, &rq_flags);
			sched_enqueue_locked(target_rq, task,
					     SCHED_ENQUEUE_WAKE);
			spin_unlock_irqrestore(&target_rq->lock, rq_flags);
			notify_id = target->id;
			home_rq = NULL;
		}
	} else {
		/* Blocked, stopped, or not yet enqueued: re-home now; wake
		 * and resume placement re-read task->cpu under the Task
		 * wait lock. */
		target = sched_select_cpu(&task->effective_affinity);
		BUG_ON(!target);
		task->cpu = target;
	}
	if (home_rq)
		spin_unlock_irqrestore(&home_rq->lock, rq_flags);
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	if (notify_id != UINT32_MAX)
		sched_notify_remote(notify_id);
	return 0;
}

cpumask_t sched_get_affinity(struct task_struct *task)
{
	cpumask_t policy_mask;
	cpumask_t mask = {0};
	irq_flags_t flags;

	if (!task)
		return mask;
	sched_policy_mask(&policy_mask);
	spin_lock_irqsave(&task->wait.lock, &flags);
	cpumask_and(&mask, &task->requested_affinity, &policy_mask);
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return mask;
}

void sched_task_allow_all_cpus(struct task_struct *task)
{
	cpumask_t policy_mask;
	irq_flags_t flags;

	if (!task)
		return;
	sched_policy_mask(&policy_mask);
	spin_lock_irqsave(&task->wait.lock, &flags);
	/* The kernel-thread default pins every constructed Task to logical
	 * CPU 0.  A kernel-origin Task that becomes a user process carries
	 * no explicit affinity choice yet, so it is widened to the full
	 * schedulable set; descendants inherit it at clone. */
	cpumask_copy(&task->requested_affinity, &policy_mask);
	cpumask_copy(&task->effective_affinity, &policy_mask);
	spin_unlock_irqrestore(&task->wait.lock, flags);
}

bool sched_wake_external(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	uint32_t cpu_id = 0;
	bool woke = false;

	if (!task || task_is_idle(task))
		return false;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	if (task->lifecycle == TASK_LIVE) {
		rq = sched_rq_for_task_locked(task);
		BUG_ON(!cpumask_test_cpu(&task->effective_affinity, rq->cpu_id));
		spin_lock_irqsave(&rq->lock, &rq_flags);
		/* The on_rq/on_cpu decision is re-validated under the
		 * runqueue lock: the switch-in publication exception dequeues
		 * and publishes the picked Task without its wait lock, so
		 * only the runqueue lock excludes that window. */
		if (!task->on_rq && !task->on_cpu) {
			task->run_state = TASK_RUNNABLE;
			sched_enqueue_locked(rq, task, SCHED_ENQUEUE_WAKE);
			woke = true;
			cpu_id = rq->cpu_id;
		}
		spin_unlock_irqrestore(&rq->lock, rq_flags);
	}
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	if (woke)
		sched_notify_remote(cpu_id);
	return woke;
}

bool sched_retired_pop(struct task_struct **task)
{
	if (!task)
		return false;
	*task = NULL;
	/* Removal is the scheduler's Retirement witness: the task was placed on
	 * this queue only after its CPU handoff completed. TASK_DEAD alone never
	 * authorizes the reaper to free the task stack. */
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

static void sched_retire_complete(struct task_struct *task)
{
	struct retired_queue *retired = &retired_queues[current_cpu()->id];
	irq_flags_t flags;

	spin_lock_irqsave(&retired->lock, &flags);
	BUG_ON(task->on_cpu || !list_empty(&task->retired_node));
	list_add_tail(&task->retired_node, &retired->tasks);
	spin_unlock_irqrestore(&retired->lock, flags);
}

bool sched_stop(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	bool stopped = false;

	if (!task || task_is_idle(task))
		return false;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	if (task->lifecycle == TASK_LIVE) {
		/* A queued Task is dequeued from the runqueue it actually
		 * sits on -- task->cpu's -- never from a reselected one. */
		rq = sched_rq_for_task(task);
		spin_lock_irqsave(&rq->lock, &rq_flags);
		if (task->run_state != TASK_STOPPED) {
			if (task->on_rq)
				sched_dequeue_locked(rq, task);
			task->run_state = TASK_STOPPED;
			stopped = true;
		}
		spin_unlock_irqrestore(&rq->lock, rq_flags);
	}
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	return stopped;
}

bool sched_resume(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	uint32_t cpu_id = UINT32_MAX;
	bool resumed = false;

	if (!task || task_is_idle(task))
		return false;
	spin_lock_irqsave(&task->wait.lock, &wait_flags);
	if (task->lifecycle == TASK_LIVE && task->run_state == TASK_STOPPED) {
		/* See sched_wake: a mid-save Task stays unqueued until Switch
		 * Completion publishes it non-active. */
		if (task->on_cpu) {
			task->run_state = TASK_RUNNABLE;
			resumed = true;
		} else {
			rq = sched_rq_for_task_locked(task);
			BUG_ON(!cpumask_test_cpu(&task->effective_affinity,
						 rq->cpu_id));
			spin_lock_irqsave(&rq->lock, &rq_flags);
			task->run_state = TASK_RUNNABLE;
			if (!task->on_rq)
				sched_enqueue_locked(rq, task,
						     SCHED_ENQUEUE_WAKE);
			resumed = true;
			cpu_id = rq->cpu_id;
			spin_unlock_irqrestore(&rq->lock, rq_flags);
		}
	}
	spin_unlock_irqrestore(&task->wait.lock, wait_flags);
	if (cpu_id != UINT32_MAX)
		sched_notify_remote(cpu_id);
	return resumed;
}

bool sched_has_runnable(void)
{
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	return rq->nr_running != 0;
}

/*
 * Switch-out work performed by Switch Completion after the
 * architecture handoff returned: the predecessor's context is fully
 * saved and the successor's context is running.  The on_cpu witness is
 * cleared under the Task's wait lock, which also excludes every
 * placement agent while the re-home decision is made.
 *
 * A Task that was Migration-Pending when it switched out is re-homed here,
 * now that its context can no longer be dispatched mid-save. A wake or
 * resume that arrived mid-save left the Task runnable but unqueued; it is
 * placed here. All placement takes one runqueue lock at a time, and a
 * placement agent that won the wait lock first has already updated the
 * Task's run state; completion defers to that state.
 */
static void sched_switch_out_complete(struct task_struct *prev, bool terminal)
{
	struct runqueue *target_rq;
	struct cpu *target;
	irq_flags_t wait_flags;
	irq_flags_t rq_flags;
	uint32_t notify_id = UINT32_MAX;

	if (task_is_idle(prev))
		return;
	spin_lock_irqsave(&prev->wait.lock, &wait_flags);
	BUG_ON(!prev->on_cpu || prev->on_rq);
	prev->on_cpu = false;
	if (!terminal && prev->lifecycle == TASK_LIVE &&
	    (prev->run_state == TASK_RUNNING ||
	     prev->run_state == TASK_RUNNABLE)) {
		if (!prev->on_rq) {
			target = sched_select_cpu(&prev->effective_affinity);
			BUG_ON(!target);
			target_rq = sched_rq_for_cpu(target);
			spin_lock_irqsave(&target_rq->lock, &rq_flags);
			sched_enqueue_locked(target_rq, prev,
					     prev->run_state == TASK_RUNNABLE ?
					     SCHED_ENQUEUE_WAKE :
					     SCHED_ENQUEUE_PREEMPT);
			spin_unlock_irqrestore(&target_rq->lock, rq_flags);
			notify_id = target->id;
		}
	}
	spin_unlock_irqrestore(&prev->wait.lock, wait_flags);
	if (terminal) {
		BUG_ON(prev->lifecycle != TASK_DEAD ||
		       prev->run_state != TASK_STOPPED || prev->on_rq);
		sched_retire_complete(prev);
	}
	if (notify_id != UINT32_MAX)
		sched_notify_remote(notify_id);
}

static void sched_handoff_begin_locked(struct runqueue *rq,
				       struct task_struct *outgoing,
				       struct task_struct *incoming,
				       bool terminal)
{
	BUG_ON(rq->handoff.pending || !outgoing || !incoming ||
	       outgoing == incoming);
	/* CPU-current structural ownership guarantees a live reference count;
	 * the transaction adds a lifecycle reference for the interval after
	 * on_cpu is cleared and placement or Retirement becomes visible. */
	BUG_ON(!task_try_get(outgoing));
	rq->handoff.outgoing = outgoing;
	rq->handoff.incoming = incoming;
	rq->handoff.incoming_mm = NULL;
	rq->handoff.installed_pgroot = 0;
	rq->handoff.terminal = terminal;
	rq->handoff.pending = true;
}

static uintptr_t sched_handoff_prepare_mm(struct runqueue *rq)
{
	struct task_struct *incoming = rq->handoff.incoming;
	struct mm_struct *mm;

	BUG_ON(!rq->handoff.pending || !incoming ||
	       rq->handoff.incoming_mm || rq->handoff.installed_pgroot);
	mm = !task_is_idle(incoming) && incoming->proc ?
		proc_mm_get(incoming->proc) : NULL;
	rq->handoff.incoming_mm = mm;
	rq->handoff.installed_pgroot = mm ? mm_pgroot(mm) : kernel_pgroot();
	return rq->handoff.installed_pgroot;
}

static void sched_switch_complete(struct task_struct *last)
{
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	struct mm_struct *incoming_mm;
	struct mm_struct *oldmm;
	struct mm_struct *dropmm;
	bool terminal;
	irq_flags_t flags;

	BUG_ON(!irqs_disabled() || in_irq() || spinlock_held());
	spin_lock_irqsave(&rq->lock, &flags);
	BUG_ON(!rq->handoff.pending || !last ||
	       last != rq->handoff.outgoing ||
	       current_task() != rq->handoff.incoming ||
	       rq->current != rq->handoff.incoming ||
	       csr_read(satp) != rq->handoff.installed_pgroot);
	incoming_mm = rq->handoff.incoming_mm;
	oldmm = rq->active_mm;
	terminal = rq->handoff.terminal;
	spin_unlock_irqrestore(&rq->lock, flags);

	sched_switch_out_complete(last, terminal);

	spin_lock_irqsave(&rq->lock, &flags);
	BUG_ON(!rq->handoff.pending || rq->handoff.outgoing != last ||
	       rq->handoff.incoming != current_task() ||
	       rq->current != current_task() ||
	       (!task_is_idle(current_task()) && !current_task()->on_cpu) ||
	       csr_read(satp) != rq->handoff.installed_pgroot);
	if (oldmm != incoming_mm) {
		/* Transfer the transaction's incoming-MM reference into the
		 * Active MM publication. */
		rq->active_mm = incoming_mm;
		dropmm = oldmm;
	} else {
		/* The existing publication already owns the same MM. */
		dropmm = incoming_mm;
	}
	rq->handoff.outgoing = NULL;
	rq->handoff.incoming = NULL;
	rq->handoff.incoming_mm = NULL;
	rq->handoff.installed_pgroot = 0;
	rq->handoff.terminal = false;
	rq->handoff.pending = false;
	BUG_ON(rq->current != current_task() ||
	       (task_is_idle(current_task()) && rq->active_mm) ||
	       (rq->active_mm && csr_read(satp) != mm_pgroot(rq->active_mm)) ||
	       (!rq->active_mm && csr_read(satp) != kernel_pgroot()));
	spin_unlock_irqrestore(&rq->lock, flags);
	mm_put(dropmm);
	task_put(last);
}

void sched_first_dispatch(struct task_struct *last)
{
	struct task_struct *task = current_task();

	BUG_ON(!task || task_is_idle(task) || !task->arch.tf ||
	       !irqs_disabled());
	sched_switch_complete(last);
	trapret_to_user(task->arch.tf);
	unreachable();
}

/*
 * The one switch core behind live and terminal entries. IRQ state is restored
 * to the state saved by the incoming Task's suspended scheduler frame. A Task
 * without such a continuation enters sched_first_dispatch() instead.
 */
static void sched_switch_core(bool terminal)
{
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	struct task_struct *prev = current_task();
	struct task_struct *next;
	struct task_struct *last;
	uintptr_t next_pgroot;
	irq_flags_t flags;

	flags = local_irq_save();
	/* Re-enqueueing the preempted Task is a placement decision, so it
	 * runs under the Task's wait lock with the runqueue lock taken
	 * after it.  Switch-in publication below is the one exception and
	 * stays under the runqueue lock alone. */
	spin_lock(&prev->wait.lock);
	spin_lock(&rq->lock);
	task_set_need_resched(prev, 0);
	/* Secondary idle services timer/IPI only; a non-empty queue here is an
	 * invariant violation, not work to dispatch from the idle loop. */
	if (!cpu_is_schedulable(rq->cpu_id)) {
		BUG_ON(prev != rq->idle);
		BUG_ON(rq->nr_running != 0);
		spin_unlock(&rq->lock);
		spin_unlock(&prev->wait.lock);
		local_irq_restore(flags);
		return;
	}
	if (terminal) {
		BUG_ON(prev == rq->idle || prev->lifecycle != TASK_DEAD ||
		       prev->on_rq || !prev->on_cpu);
		prev->run_state = TASK_STOPPED;
	}
	next = sched_pick_locked(rq);
	if (!next && !terminal && prev != rq->idle &&
	    prev->lifecycle == TASK_LIVE &&
	    prev->run_state == TASK_RUNNING &&
	    !sched_task_migration_pending(prev))
		next = prev;
	if (!next)
		next = rq->idle;
	sched_switch_locked(rq, next);
	if (next != prev)
		sched_handoff_begin_locked(rq, prev, next, terminal);
	spin_unlock(&rq->lock);
	spin_unlock(&prev->wait.lock);
	if (next == prev) {
		BUG_ON(terminal);
		local_irq_restore(flags);
		return;
	}
	next_pgroot = sched_handoff_prepare_mm(rq);
	last = sched_handoff(prev, next, next_pgroot);
	sched_switch_complete(last);
	local_irq_restore(flags);
}

void schedule(void)
{
	BUG_ON(in_irq());
	BUG_ON(!current_task());
	BUG_ON(!preemptible());
	BUG_ON(spinlock_held());
	BUG_ON(irqs_disabled());

	sched_switch_core(false);
}

void schedule_irqoff(void)
{
	/* The trap-return path owns IRQ restoration after this handoff. */
	BUG_ON(!irqs_disabled());
	BUG_ON(in_irq());
	BUG_ON(!current_task());
	BUG_ON(!preemptible());
	BUG_ON(spinlock_held());

	sched_switch_core(false);
}

__noreturn
void sched_exit_current(void)
{
	BUG_ON(!current_task() || task_is_idle(current_task()));
	BUG_ON(current_task()->lifecycle != TASK_DEAD);
	BUG_ON(in_irq() || !preemptible() || spinlock_held());
	sched_switch_core(true);
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

bool sched_cpu_mm_targets(uint32_t cpu_id, struct mm_struct *mm)
{
	struct runqueue *rq = &runqueues[cpu_id];
	bool target;
	irq_flags_t flags;

	spin_lock_irqsave(&rq->lock, &flags);
	target = rq->handoff.pending || rq->active_mm == mm;
	spin_unlock_irqrestore(&rq->lock, flags);
	return target;
}

void sched_publish_active_mm(struct mm_struct *mm)
{
	struct runqueue *rq = sched_rq_for_cpu(current_cpu());
	struct mm_struct *oldmm;
	irq_flags_t flags;

	if (mm)
		mm_get(mm);
	spin_lock_irqsave(&rq->lock, &flags);
	BUG_ON(rq->handoff.pending);
	oldmm = rq->active_mm;
	if (oldmm == mm) {
		spin_unlock_irqrestore(&rq->lock, flags);
		mm_put(mm);
		return;
	}
	rq->active_mm = mm;
	spin_unlock_irqrestore(&rq->lock, flags);
	mm_put(oldmm);
}

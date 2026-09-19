#include <nuvix/ipi.h>
#include <nuvix/mm.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/trap.h>

#define TIME_SLICE (5 * timer_tick_interval)

struct runqueue {
	spinlock_t lock;
	struct list_head ready;
	struct list_head retired;
	struct task_struct *current;
	struct task_struct *idle;
	struct mm_struct *active_mm;
	struct mm_struct *old_mm;
	atomic_t load;
	uint32_t id;
	IFDEF(CONFIG_SMP, uint64_t balance_due;)
};

struct affinity_waiter {
	struct list_head node;
	struct task_struct *task;
	uint64_t generation;
	uint64_t sequence;
};

static struct runqueue runqueues[NR_CPUS];
static struct task_struct *reaper;
static atomic_t reap_pending = ATOMIC_INIT(1);

static struct runqueue *home(struct task_struct *task)
{
	struct cpu *cpu = compiler_atomic_load_n(&task->cpu,
						COMPILER_ATOMIC_ACQUIRE);
	return &runqueues[cpu->id];
}

static struct runqueue *lock_task(struct task_struct *task, irq_flags_t *flags)
{
	struct runqueue *rq;

	for (;;) {
		rq = home(task);
		spin_lock_irqsave(&rq->lock, flags);
		if (rq == home(task))
			return rq;
		spin_unlock_irqrestore(&rq->lock, *flags);
	}
}

static void kick(uint32_t cpu)
{
#ifdef CONFIG_SMP
	if (cpu != current_cpu()->id)
		(void)ipi_send(cpu);
#else
	BUG_ON(cpu != current_cpu()->id);
#endif
}

static uint32_t select_cpu(const cpumask_t *mask, uint32_t preferred)
{
	uint32_t best = preferred;
	int32_t load = INT32_MAX;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		int32_t n;

		if (!cpu_is_schedulable(id) || !cpumask_test_cpu(mask, id))
			continue;
		n = atomic_read_relaxed(&runqueues[id].load);
		if (n < load || (n == load && id == preferred)) {
			load = n;
			best = id;
		}
	}
	return best;
}

static void enqueue(struct runqueue *rq, struct task_struct *task)
{
	BUG_ON(!list_empty(&task->sched.run_node));
	task->run_state = TASK_RUNNABLE;
	list_add_tail(&task->sched.run_node, &rq->ready);
	atomic_add_fetch_relaxed(&rq->load, 1);
	task_set_need_resched(rq->current, 1);
}

static void dequeue(struct runqueue *rq, struct task_struct *task)
{
	list_del_init(&task->sched.run_node);
	atomic_sub_fetch_relaxed(&rq->load, 1);
}

static void wake_locked(struct runqueue *rq, struct task_struct *task)
{
	if (rq->current == task) {
		task->run_state = TASK_RUNNING;
	} else {
		enqueue(rq, task);
	}
}

static void affinity_complete(struct task_struct *task)
{
	for (;;) {
		struct affinity_waiter *waiter;
		struct task_struct *caller;
		uint64_t generation;
		irq_flags_t flags;
		struct runqueue *rq = lock_task(task, &flags);

		if (list_empty(&task->sched.affinity_waiters)) {
			spin_unlock_irqrestore(&rq->lock, flags);
			return;
		}
		waiter = list_first_entry(&task->sched.affinity_waiters,
					 struct affinity_waiter, node);
		if (waiter->sequence == task->sched.affinity_sequence &&
		    !cpumask_test_cpu(&task->allowed_cpus, rq->id) &&
		    task->run_state != TASK_RETIRED) {
			spin_unlock_irqrestore(&rq->lock, flags);
			return;
		}
		caller = waiter->task;
		generation = waiter->generation;
		BUG_ON(!task_try_get(caller));
		list_del_init(&waiter->node);
		spin_unlock_irqrestore(&rq->lock, flags);
		(void)sched_wake(caller, generation);
		task_put(caller);
	}
}

static void rehome(struct task_struct *task)
{
	for (;;) {
		struct runqueue *src, *dst, *first, *second;
		uint32_t target;
		irq_flags_t flags;
		bool ready;

		src = lock_task(task, &flags);
		if (task->run_state == TASK_RETIRED ||
		    cpumask_test_cpu(&task->allowed_cpus, src->id)) {
			if (task->run_state == TASK_MIGRATING)
				enqueue(src, task);
			spin_unlock_irqrestore(&src->lock, flags);
			break;
		}
		if (src->current == task) {
			task_set_need_resched(task, 1);
			spin_unlock_irqrestore(&src->lock, flags);
			kick(src->id);
			return;
		}
		target = select_cpu(&task->allowed_cpus, src->id);
		spin_unlock_irqrestore(&src->lock, flags);
		dst = &runqueues[target];
		first = src->id < dst->id ? src : dst;
		second = src->id < dst->id ? dst : src;
		flags = local_irq_save();
		spin_lock(&first->lock);
		if (second != first)
			spin_lock(&second->lock);
		if (home(task) != src || src->current == task ||
		    !cpumask_test_cpu(&task->allowed_cpus, target)) {
			if (second != first)
				spin_unlock(&second->lock);
			spin_unlock(&first->lock);
			local_irq_restore(flags);
			continue;
		}
		ready = task->run_state == TASK_RUNNABLE ||
			task->run_state == TASK_MIGRATING;
		if (task->run_state == TASK_RUNNABLE)
			dequeue(src, task);
		compiler_atomic_store_n(&task->cpu, &cpu_table[target],
					COMPILER_ATOMIC_RELEASE);
		if (ready)
			enqueue(dst, task);
		if (second != first)
			spin_unlock(&second->lock);
		spin_unlock(&first->lock);
		local_irq_restore(flags);
		if (ready)
			kick(target);
		break;
	}
	affinity_complete(task);
}

void sched_task_init(struct task_struct *task)
{
	INIT_LIST_HEAD(&task->sched.run_node);
	INIT_LIST_HEAD(&task->sched.affinity_waiters);
	atomic_set(&task->sched.need_resched, 0);
	task->sched.slice_left = TIME_SLICE;
	task->cpu = &cpu_table[0];
	cpumask_zero(&task->allowed_cpus);
	for (uint32_t id = 0; id < nr_cpu_ids; id++)
		cpumask_set_cpu(&task->allowed_cpus, id);
}

void sched_init(void)
{
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		struct runqueue *rq = &runqueues[id];

		spin_lock_init(&rq->lock, LOCK_RANK_RUNQUEUE,
			       LOCK_IRQ_HARDIRQ_REACHABLE);
		INIT_LIST_HEAD(&rq->ready);
		INIT_LIST_HEAD(&rq->retired);
		rq->id = id;
		rq->idle = cpu_table[id].idle_task;
		rq->current = cpu_table[id].current_task;
		atomic_set(&rq->load, 0);
	}
}

void sched_enqueue_new(struct task_struct *task)
{
	struct runqueue *rq;
	irq_flags_t flags;
	uint32_t cpu = select_cpu(&task->allowed_cpus, current_cpu()->id);

	/* The constructor alone places DORMANT tasks. */
	rq = &runqueues[cpu];
	spin_lock_irqsave(&rq->lock, &flags);
	BUG_ON(task->run_state != TASK_DORMANT);
	compiler_atomic_store_n(&task->cpu, &cpu_table[cpu],
				COMPILER_ATOMIC_RELEASE);
	enqueue(rq, task);
	spin_unlock_irqrestore(&rq->lock, flags);
	kick(cpu);
}

uint64_t sched_park_arm(struct task_struct *task,
			enum task_wait_policy policy, uint64_t signal_set)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);

	BUG_ON(task != current_task() || task->sched.park_active);
	uint64_t generation = (uint64_t)atomic64_add_fetch_relaxed(
		&task->sched.park_generation, 1);
	task->sched.park_active = true;
	task->sched.notified = false;
	task->sched.event_fired = false;
	task->sched.park_policy = policy;
	task->sched.park_signal_set = signal_set;
	spin_unlock_irqrestore(&rq->lock, flags);
	return generation;
}

void sched_park_finish(struct task_struct *task)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);

	task->sched.park_active = false;
	task->sched.notified = false;
	spin_unlock_irqrestore(&rq->lock, flags);
}

static bool notify(struct task_struct *task, uint64_t generation,
		   bool event, bool signal, bool fatal)
{
	irq_flags_t flags;
	struct runqueue *rq;
	bool wake = false;

	if (!task || task_is_idle(task))
		return false;
	rq = lock_task(task, &flags);
	if (task->sched.park_active &&
	    (signal || (uint64_t)atomic64_read_relaxed(
				&task->sched.park_generation) == generation) &&
	    (!signal || task->sched.park_policy == TASK_WAIT_INTERRUPTIBLE ||
	     (fatal && task->sched.park_policy == TASK_WAIT_KILLABLE))) {
		task->sched.notified = true;
		task->sched.event_fired |= event;
		if (task->run_state == TASK_BLOCKED) {
			wake_locked(rq, task);
			wake = true;
		}
	}
	spin_unlock_irqrestore(&rq->lock, flags);
	if (wake)
		kick(rq->id);
	return wake;
}

bool sched_wake(struct task_struct *task, uint64_t generation)
{
	return notify(task, generation, false, false, false);
}

bool sched_wake_event(struct task_struct *task, uint64_t generation)
{
	return notify(task, generation, true, false, false);
}

bool sched_wake_signal(struct task_struct *task, bool fatal)
{
	return notify(task, 0, false, true, fatal);
}

bool sched_take_event(struct task_struct *task)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);
	bool event = task->sched.event_fired;

	task->sched.event_fired = false;
	spin_unlock_irqrestore(&rq->lock, flags);
	return event;
}

bool sched_wait_accepts_signal(struct task_struct *task, uint64_t mask)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);
	bool accepts = task->sched.park_active &&
		(task->sched.park_signal_set & mask) != 0;

	spin_unlock_irqrestore(&rq->lock, flags);
	return accepts;
}

enum sched_park_result sched_block_current(uint64_t generation)
{
	struct task_struct *task = current_task();
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);

	BUG_ON(!task->sched.park_active ||
	       (uint64_t)atomic64_read_relaxed(
		       &task->sched.park_generation) != generation);
	if (task->sched.notified) {
		task->sched.notified = false;
		spin_unlock_irqrestore(&rq->lock, flags);
		return SCHED_PARK_RACE;
	}
	BUG_ON(rq->current != task || task->run_state != TASK_RUNNING);
	task->run_state = TASK_BLOCKED;
	spin_unlock_irqrestore(&rq->lock, flags);
	schedule();
	return SCHED_PARKED;
}

bool sched_stop(struct task_struct *task)
{
	irq_flags_t flags;
	struct runqueue *rq;

	BUG_ON(task != current_task());
	rq = lock_task(task, &flags);
	task->run_state = TASK_STOPPED;
	spin_unlock_irqrestore(&rq->lock, flags);
	return true;
}

bool sched_resume(struct task_struct *task)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);
	bool stopped = task->run_state == TASK_STOPPED;

	if (stopped)
		wake_locked(rq, task);
	spin_unlock_irqrestore(&rq->lock, flags);
	if (stopped)
		kick(rq->id);
	return stopped;
}

bool sched_task_stopped(struct task_struct *task)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);
	bool stopped = task->run_state == TASK_STOPPED;

	spin_unlock_irqrestore(&rq->lock, flags);
	return stopped;
}

#ifdef CONFIG_SMP
static void balance(void)
{
	struct runqueue *dst = &runqueues[current_cpu()->id];
	uint32_t victim = dst->id;
	int32_t load = atomic_read_relaxed(&dst->load) + 1;
	uint64_t now = timer_now();

	if (!cpu_is_schedulable(dst->id))
		return;
	if (load > 1 && now < dst->balance_due)
		return;
	dst->balance_due = now + 20 * timer_tick_interval;
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		int32_t n = atomic_read_relaxed(&runqueues[id].load);

		if (cpu_is_schedulable(id) && n > load) {
			victim = id;
			load = n;
		}
	}
	if (victim != dst->id) {
		struct runqueue *src = &runqueues[victim];
		struct runqueue *first = src->id < dst->id ? src : dst;
		struct runqueue *second = src->id < dst->id ? dst : src;
		struct task_struct *task;
		irq_flags_t flags = local_irq_save();

		spin_lock(&first->lock);
		spin_lock(&second->lock);
		list_for_each_entry(task, &src->ready, sched.run_node) {
			if (!cpumask_test_cpu(&task->allowed_cpus, dst->id))
				continue;
			dequeue(src, task);
			compiler_atomic_store_n(&task->cpu, &cpu_table[dst->id],
						COMPILER_ATOMIC_RELEASE);
			enqueue(dst, task);
			break;
		}
		spin_unlock(&second->lock);
		spin_unlock(&first->lock);
		local_irq_restore(flags);
	}
}

#endif

void sched_notify_reaper(void)
{
	struct task_struct *task;

	atomic_set_release(&reap_pending, 1);
	task = compiler_atomic_load_n(&reaper, COMPILER_ATOMIC_ACQUIRE);
	if (task) {
		uint64_t generation =
			(uint64_t)atomic64_read_acquire(
				&task->sched.park_generation);

		(void)sched_wake(task, generation);
	}
}

void sched_reaper_sleep(void)
{
	struct task_struct *task = current_task();
	uint64_t generation;

	compiler_atomic_store_n(&reaper, task, COMPILER_ATOMIC_RELEASE);
	generation = sched_park_arm(task, TASK_WAIT_UNINTERRUPTIBLE, 0);
	if (!atomic_xchg_acquire(&reap_pending, 0))
		(void)sched_block_current(generation);
	sched_park_finish(task);
}

bool sched_retired_pop(struct task_struct **task)
{
	*task = NULL;
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		struct runqueue *rq = &runqueues[id];
		irq_flags_t flags;

		spin_lock_irqsave(&rq->lock, &flags);
		if (!list_empty(&rq->retired)) {
			*task = list_first_entry(&rq->retired, struct task_struct,
						retired_node);
			list_del_init(&(*task)->retired_node);
		}
		spin_unlock_irqrestore(&rq->lock, flags);
		if (*task)
			return true;
	}
	return false;
}

static void switch_finish(struct task_struct *last)
{
	struct runqueue *rq = &runqueues[current_cpu()->id];
	struct mm_struct *old = rq->old_mm;
	bool retired = last->run_state == TASK_RETIRED;

	rq->old_mm = NULL;
	if (retired)
		list_add_tail(&last->retired_node, &rq->retired);
	spin_unlock(&rq->lock);
	mm_put(old);
	if (!task_is_idle(last)) {
		if (retired) {
			sched_notify_reaper();
		} else {
			rehome(last);
		}
		affinity_complete(last);
		task_put(last);
	}
}

void sched_first_dispatch(struct task_struct *last)
{
	switch_finish(last);
	trapret_to_user(current_task()->arch.tf);
	unreachable();
}

static void switch_core(bool terminal)
{
	struct runqueue *rq;
	struct task_struct *prev, *next;
	struct mm_struct *mm;
	uint64_t now, used;
	irq_flags_t flags = local_irq_save();

	rq = &runqueues[current_cpu()->id];
	spin_lock(&rq->lock);
	prev = rq->current;
	BUG_ON(prev != current_task());
	task_set_need_resched(prev, 0);
	now = timer_now();
	if (!task_is_idle(prev)) {
		used = now - prev->sched.runtime_start;
		prev->sched.slice_left = used >= prev->sched.slice_left ?
			0 : prev->sched.slice_left - used;
		atomic_sub_fetch_relaxed(&rq->load, 1);
		if (terminal)
			prev->run_state = TASK_RETIRED;
		else if (prev->run_state == TASK_RUNNING) {
			if (cpumask_test_cpu(&prev->allowed_cpus, rq->id))
				enqueue(rq, prev);
			else
				prev->run_state = TASK_MIGRATING;
		}
	}
	next = list_empty(&rq->ready) ? rq->idle :
		list_first_entry(&rq->ready, struct task_struct, sched.run_node);
	if (next != rq->idle) {
		dequeue(rq, next);
		atomic_add_fetch_relaxed(&rq->load, 1);
		next->run_state = TASK_RUNNING;
		if (!next->sched.slice_left)
			next->sched.slice_left = TIME_SLICE;
		next->sched.runtime_start = now;
		task_set_need_resched(next, 0);
	}
	if (prev == next) {
		spin_unlock(&rq->lock);
		local_irq_restore(flags);
		return;
	}
	if (!task_is_idle(prev))
		BUG_ON(!task_try_get(prev));
	mm = next->mm;
	mm_get(mm);
	rq->old_mm = rq->active_mm;
	rq->active_mm = mm;
	rq->current = next;
	set_current_task(next);
	/* rq stays CPU-owned until the new stack completes this handoff. */
	prev = arch_task_switch(prev, next, mm);
	switch_finish(prev);
	local_irq_restore(flags);
}

void schedule(void)
{
	BUG_ON(!sched_context_can_schedule() || irqs_disabled());
	IFDEF(CONFIG_SMP, balance();)
	switch_core(false);
}

void schedule_irqoff(void)
{
	BUG_ON(!sched_context_can_schedule() || !irqs_disabled());
	switch_core(false);
}

void sched_exit_current(void)
{
	BUG_ON(!sched_context_can_schedule() || task_is_idle(current_task()));
	switch_core(true);
	unreachable();
}

void sched_request(void)
{
	if (current_task())
		task_set_need_resched(current_task(), 1);
}

void sched_tick(void)
{
	struct task_struct *task = current_task();
	struct runqueue *rq = &runqueues[current_cpu()->id];
	irq_flags_t flags;

	if (!task)
		return;
	spin_lock_irqsave(&rq->lock, &flags);
	if (!task_is_idle(task)) {
		if (task_trap_frome_user(task))
			task->cputime.utime_ticks++;
		else
			task->cputime.stime_ticks++;
		if ((!list_empty(&rq->ready) &&
		     timer_now() - task->sched.runtime_start >= task->sched.slice_left)
		    IFDEF(CONFIG_SMP, || timer_now() >= rq->balance_due))
			task_set_need_resched(task, 1);
	} else if (!list_empty(&rq->ready)) {
		task_set_need_resched(task, 1);
	}
	spin_unlock_irqrestore(&rq->lock, flags);
}

void sched_yield(void)
{
	sched_request();
	schedule();
}

bool sched_has_runnable(void)
{
	return atomic_read_relaxed(&runqueues[current_cpu()->id].load) != 0;
}

void sched_idle(void)
{
	irq_flags_t flags = local_irq_save();

	if (!sched_has_runnable() && !task_need_resched(current_task()))
		wait_for_interrupt();
	local_irq_restore(flags);
}

void sched_activate_mm(struct mm_struct *mm)
{
	struct runqueue *rq;
	struct mm_struct *old;
	irq_flags_t flags = local_irq_save();

	mm_get(mm);
	rq = &runqueues[current_cpu()->id];
	spin_lock(&rq->lock);
	old = rq->active_mm;
	activate_mm(mm);
	rq->active_mm = mm;
	spin_unlock(&rq->lock);
	local_irq_restore(flags);
	mm_put(old);
}

int sched_set_affinity(struct task_struct *task, const cpumask_t *requested)
{
	struct task_struct *caller = current_task();
	struct affinity_waiter waiter;
	struct runqueue *rq;
	cpumask_t mask = {0};
	irq_flags_t flags;
	uint64_t generation;

	for (uint32_t id = 0; id < nr_cpu_ids; id++)
		if (cpu_is_schedulable(id) && cpumask_test_cpu(requested, id))
			cpumask_set_cpu(&mask, id);
	if (cpumask_empty(&mask))
		return -EINVAL;
	INIT_LIST_HEAD(&waiter.node);
	waiter.task = caller;
	generation = sched_park_arm(caller, TASK_WAIT_UNINTERRUPTIBLE, 0);
	waiter.generation = generation;
	rq = lock_task(task, &flags);
	if (task->run_state == TASK_RETIRED) {
		spin_unlock_irqrestore(&rq->lock, flags);
		sched_park_finish(caller);
		return -ESRCH;
	}
	task->allowed_cpus = mask;
	waiter.sequence = ++task->sched.affinity_sequence;
	list_add_tail(&waiter.node, &task->sched.affinity_waiters);
	spin_unlock_irqrestore(&rq->lock, flags);
	affinity_complete(task);
	rehome(task);
	for (;;) {
		bool done;

		rq = lock_task(task, &flags);
		done = list_empty(&waiter.node);
		spin_unlock_irqrestore(&rq->lock, flags);
		if (done)
			break;
		(void)sched_block_current(generation);
	}
	sched_park_finish(caller);
	return 0;
}

cpumask_t sched_get_affinity(struct task_struct *task)
{
	irq_flags_t flags;
	struct runqueue *rq = lock_task(task, &flags);
	cpumask_t mask = task->allowed_cpus;

	spin_unlock_irqrestore(&rq->lock, flags);
	return mask;
}

void sched_task_inherit_affinity(struct task_struct *child,
				struct task_struct *parent)
{
	child->allowed_cpus = sched_get_affinity(parent);
}

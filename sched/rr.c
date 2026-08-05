/* sched/rr.c - fixed-quantum FIFO round-robin policy */

#include <kernel/task.h>

#include "internal.h"

#define RR_QUANTUM_TICKS 5

struct rr_runqueue_state {
	struct task_struct *current;
	uint32_t ticks;
};

static struct rr_runqueue_state rr_runqueues[NR_CPUS];

static struct rr_runqueue_state *rr_state(const struct runqueue *rq)
{
	BUG_ON(!rq || rq->cpu_id >= NR_CPUS);
	return &rr_runqueues[rq->cpu_id];
}

static void rr_init(void)
{
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		rr_runqueues[id].current = NULL;
		rr_runqueues[id].ticks = 0;
	}
}

static void rr_enqueue(struct runqueue *rq, struct task_struct *task,
		       enum sched_enqueue_reason reason)
{
	BUG_ON(!rq || !task || task->on_rq);
	(void)reason;
	list_add_tail(&task->sched.run_node, &rq->runnable);
}

static void rr_dequeue(struct runqueue *rq, struct task_struct *task)
{
	BUG_ON(!rq || !task || !task->on_rq);
	list_del_init(&task->sched.run_node);
}

static struct task_struct *rr_pick_next(struct runqueue *rq)
{
	struct rr_runqueue_state *state;
	struct task_struct *task;

	if (!rq || list_empty(&rq->runnable))
		return NULL;
	task = list_first_entry(&rq->runnable, struct task_struct,
				sched.run_node);
	state = rr_state(rq);
	state->current = task;
	state->ticks = 0;
	return task;
}

static bool rr_tick(struct runqueue *rq, struct task_struct *task)
{
	struct rr_runqueue_state *state;

	if (!rq || !task)
		return false;
	state = rr_state(rq);
	BUG_ON(state->current != task);
	if (state->ticks < RR_QUANTUM_TICKS)
		state->ticks++;
	return state->ticks >= RR_QUANTUM_TICKS;
}

const struct sched_ops rr_ops = {
	.init = rr_init,
	.enqueue = rr_enqueue,
	.dequeue = rr_dequeue,
	.pick_next = rr_pick_next,
	.tick = rr_tick,
};

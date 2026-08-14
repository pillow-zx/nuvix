#ifndef _NUVIX_SCHED_INTERNAL_H
#define _NUVIX_SCHED_INTERNAL_H

#include <nuvix/sched.h>

enum sched_enqueue_reason {
	SCHED_ENQUEUE_NEW,
	SCHED_ENQUEUE_WAKE,
	SCHED_ENQUEUE_PREEMPT,
	SCHED_ENQUEUE_YIELD,
};

struct runqueue {
	spinlock_t lock;
	uint32_t cpu_id;
	struct task_struct *current;
	struct task_struct *idle;
	struct list_head runnable;
	uint32_t nr_running;
};

struct sched_ops {
	void (*init)(void);
	void (*enqueue)(struct runqueue *rq, struct task_struct *task,
			enum sched_enqueue_reason reason);
	void (*dequeue)(struct runqueue *rq, struct task_struct *task);
	struct task_struct *(*pick_next)(struct runqueue *rq);
	bool (*tick)(struct runqueue *rq, struct task_struct *task);
};

extern const struct sched_ops rr_ops;

#endif

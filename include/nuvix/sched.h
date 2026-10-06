#ifndef _NUVIX_SCHED_H
#define _NUVIX_SCHED_H

#include <nuvix/cpu.h>
#include <nuvix/task.h>
#include <nuvix/spinlock.h>

/* Kernel continuation only; the returned task is the one just switched out. */
static __always_inline struct task *switch_to(struct task *prev,
                                              struct task *next)
{
    struct task *last = __switch_to(prev, next);

    barrier();
    return last;
}

struct rq {
    spinlock_t lock;
    struct task *curr;
    struct task *idle;
    struct list_head ready_list;
    /* Includes the ordinary current task, but never idle. */
    atomic_t nr_runnable;
    uint64_t quantum_deadline;
} __aligned(SMP_CACHE_BYTES);

extern struct rq runqueues[CONFIG_NR_CPUS];

static inline struct rq *cpu_rq(unsigned int cpu_id)
{
    return &runqueues[cpu_id];
}

/* Requires a published task held alive by the caller. */
static inline struct rq *task_rq(const struct task *task)
{
    return cpu_rq(task->cpu_id);
}

/* Requires current to be bound and the execution context to stay on this CPU. */
static inline struct rq *this_rq(void)
{
    return cpu_rq(current_cpu_id());
}

/* Ordinary sleepable thread context; diagnostic only. */
#define might_sleep()                                                          \
    do {                                                                       \
        IFDEF(CONFIG_DEBUG_CONTEXT,                                            \
              Assert(preemptible() && !cpus[current_cpu_id()].irq_depth &&     \
                         current != this_rq()->idle,                           \
                     "invalid sleep context\n"));                              \
    } while (0)

/* Boot-only, once after cpu_init() and tick_init(), before concurrent rq use. */
void sched_init(void);
/*
 * Boot-only registration of a prepared NEW kernel task. Takes a permanent
 * scheduler reference. CPU bring-up binds tp and sets idle->on_cpu before
 * scheduling; this function does not mark an offline CPU as executing.
 */
void sched_init_idle(unsigned int cpu_id, struct task *idle);

/*
 * The selected CPUs must all be ready and the online set frozen. The caller
 * owns a fully prepared NEW task; publication takes a scheduler reference.
 * Current must be bound before locking. Publication and wakeup mark an idle
 * current for rescheduling and notify remote idle CPUs after unlocking.
 */
void sched_publish(struct task *task);
/*
 * Requires a published task held alive by the caller, outside its rq lock.
 * Returns true only when an ordinary sleeping task becomes RUNNABLE.
 */
bool sched_wake(struct task *task);

/* Local ordinary current, IRQ off and rq.lock held; no load change yet. */
void sched_set_sleep_locked(struct rq *rq, enum task_state state);

/*
 * Local switch operations: IRQ off, rq.lock held, and exactly one
 * scheduler-owned preempt-disable level, using __spin_lock for rq.lock.
 * Prepare hands the debug owner to next on a real switch. Retain the lock
 * across arch_switch_to, then finish and __spin_unlock. These operations do
 * not restore IRQs or preemption.
 */
/* rotate requests yield; otherwise RUNNABLE current rotates on need_resched. */
struct task *sched_prepare_switch_locked(struct rq *rq, bool rotate);
/* Only after a real switch; prev is the task returned by arch_switch_to. */
void sched_finish_switch_locked(struct rq *rq, struct task *prev);

/*
 * Caller hands over exactly one scheduler-owned preempt-disable level, with
 * no ordinary spinlock held or hard IRQ context. Saves/restores local IRQs and
 * consumes that level on return. rotate requests yield.
 */
void sched_switch(bool rotate);

/*
 * Wait commit: no caller locks, IRQs enabled, exactly one wait-owned
 * preempt-disable level. Rechecks state under rq.lock; consumes the level and
 * restores ordinary preemption on return.
 */
void sched_park(void);

/*
 * Common post-switch tail, also called by a first task entry with a0 as prev.
 * Requires IRQ off, the inherited rq lock and preempt_count == 1. Clears
 * prev->on_cpu and unlocks only. A first entry must be prepared with ctx.ra,
 * an ABI-aligned ctx.sp and preempt_count == 1; after this call it consumes
 * that level and applies its own initial IRQ policy.
 */
void sched_finish_switch(struct task *prev);

/* IRQ handler requests rotation; the IRQ-off exit checks after irq_depth == 0. */
void sched_tick(uint64_t now);
void sched_irq_exit(void);
/* Initial per-CPU idle context, with the boot preempt-disable level retained. */
__noreturn void sched_idle(void);

#endif /* _NUVIX_SCHED_H */

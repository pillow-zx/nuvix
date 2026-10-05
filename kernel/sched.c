#include <nuvix/sched.h>
#include <nuvix/assert.h>

struct rq runqueues[CONFIG_NR_CPUS];

static DEFINE_SPINLOCK(placement_lock);
static unsigned int next_cpu;

static void enqueue_task(struct rq *rq, struct task *task)
{
    Assert(!task->on_rq && task != rq->idle,
           "cannot enqueue an idle or queued task\n");
    list_add_tail(&task->rq_node, &rq->ready_list);
    task->on_rq = true;
    if (rq->curr == rq->idle)
        rq->curr->need_resched = true;
}

static struct task *pick_next_task(struct rq *rq)
{
    struct task *task;

    if (list_empty(&rq->ready_list))
        return rq->idle;
    task = list_first_entry(&rq->ready_list, struct task, rq_node);
    list_del_init(&task->rq_node);
    task->on_rq = false;
    return task;
}

static unsigned int select_cpu(void)
{
    unsigned int cpu_id = next_cpu;
    unsigned int selected = cpu_id;
    int32_t least = atomic_read(&cpu_rq(cpu_id)->nr_runnable);

    for (unsigned int i = 1; i < nr_cpus; ++i) {
        int32_t load;

        if (++cpu_id == nr_cpus)
            cpu_id = 0;
        load = atomic_read(&cpu_rq(cpu_id)->nr_runnable);
        if (load < least) {
            selected = cpu_id;
            least = load;
        }
    }
    next_cpu = selected + 1;
    if (next_cpu == nr_cpus)
        next_cpu = 0;
    return selected;
}

void sched_init(void)
{
    spin_lock_init(&placement_lock);
    next_cpu = 0;
    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        struct rq *rq = cpu_rq(cpu_id);

        spin_lock_init(&rq->lock);
        rq->curr = NULL;
        rq->idle = NULL;
        INIT_LIST_HEAD(&rq->ready_list);
        atomic_set(&rq->nr_runnable, 0);
    }
}

void sched_init_idle(unsigned int cpu_id, struct task *idle)
{
    struct rq *rq = cpu_rq(cpu_id);

    Assert(!rq->idle && idle->state == TASK_NEW && !idle->proc,
           "idle registration requires an unregistered NEW kernel task\n");
    task_get(idle);
    idle->cpu_id = cpu_id;
    idle->state = TASK_RUNNABLE;
    idle->on_rq = false;
    idle->on_cpu = false;
    idle->need_resched = false;
    rq->idle = idle;
    rq->curr = idle;
}

void sched_publish(struct task *task)
{
    struct rq *rq;
    unsigned int cpu_id;
    unsigned long placement_flags, rq_flags;

    Assert(task->state == TASK_NEW, "task already published\n");
    spin_lock_irqsave(&placement_lock, placement_flags);
    cpu_id = select_cpu();
    rq = cpu_rq(cpu_id);
    spin_lock_irqsave(&rq->lock, rq_flags);
    task_get(task);
    task->cpu_id = cpu_id;
    task->state = TASK_RUNNABLE;
    enqueue_task(rq, task);
    atomic_inc(&rq->nr_runnable);
    spin_unlock_irqrestore(&rq->lock, rq_flags);
    spin_unlock_irqrestore(&placement_lock, placement_flags);
}

bool sched_wake(struct task *task)
{
    struct rq *rq = task_rq(task);
    unsigned long flags;
    bool woke = false;

    spin_lock_irqsave(&rq->lock, flags);
    if (task->state == TASK_SLEEP_INTERRUPTIBLE ||
        task->state == TASK_SLEEP_UNINTERRUPTIBLE) {
        task->state = TASK_RUNNABLE;
        if (!task->on_cpu) {
            enqueue_task(rq, task);
            atomic_inc(&rq->nr_runnable);
        }
        woke = true;
    }
    spin_unlock_irqrestore(&rq->lock, flags);
    return woke;
}

void sched_set_sleep_locked(struct rq *rq, enum task_state state)
{
    assert_spin_locked(&rq->lock);
    Assert(rq->curr != rq->idle && (state == TASK_SLEEP_INTERRUPTIBLE ||
                                    state == TASK_SLEEP_UNINTERRUPTIBLE),
           "only an ordinary current task can register sleep\n");
    rq->curr->state = state;
}

struct task *sched_prepare_switch_locked(struct rq *rq, bool rotate)
{
    struct task *prev = rq->curr;
    struct task *next;

    assert_spin_locked(&rq->lock);
    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(irqs_disabled() && preempt_count() == 1,
                                       "invalid scheduling context\n"));
    if (prev->state != TASK_RUNNABLE) {
        Assert(prev->state == TASK_SLEEP_INTERRUPTIBLE ||
                   prev->state == TASK_SLEEP_UNINTERRUPTIBLE,
               "stop and exit require their lifecycle paths\n");
        atomic_dec(&rq->nr_runnable);
    } else {
        if (list_empty(&rq->ready_list)) {
            prev->need_resched = false;
            return prev;
        }
        if (prev != rq->idle) {
            if (!rotate && !prev->need_resched)
                return prev;
            enqueue_task(rq, prev);
        }
    }

    prev->need_resched = false;
    next = pick_next_task(rq);
    rq->curr = next;
    next->on_cpu = true;
    __spin_handoff(&rq->lock, next);
    return next;
}

void sched_finish_switch_locked(struct rq *rq, struct task *prev)
{
    assert_spin_locked(&rq->lock);
    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(irqs_disabled() && preempt_count() == 1,
                                       "invalid switch finish context\n"));
    Assert(prev != rq->curr, "switch finish requires a real switch\n");
    prev->on_cpu = false;
}

void sched_finish_switch(struct task *prev)
{
    struct rq *rq = this_rq();

    sched_finish_switch_locked(rq, prev);
    __spin_unlock(&rq->lock);
}

void sched_switch(bool rotate)
{
    unsigned long flags = local_irq_save();
    struct rq *rq = this_rq();
    struct task *prev;
    struct task *next;

    __spin_lock(&rq->lock);
    prev = rq->curr;
    next = sched_prepare_switch_locked(rq, rotate);
    if (prev != next) {
        prev = switch_to(prev, next);
        sched_finish_switch(prev);
    } else {
        __spin_unlock(&rq->lock);
    }
    local_irq_restore(flags);
    preempt_enable_no_resched();
}

#include <nuvix/sched.h>
#include <nuvix/assert.h>
#include <nuvix/tick.h>
#include <nuvix/mm.h>
#include <nuvix/pgtable.h>

struct rq runqueues[CONFIG_NR_CPUS];

static DEFINE_SPINLOCK(placement_lock);
static unsigned int next_cpu;
static uint64_t quantum_ticks;

static void enqueue_task(struct rq *rq, struct task *task)
{
    Assert(!task->on_rq && task != rq->idle,
           "cannot enqueue an idle or queued task\n");
    list_add_tail(&task->rq_node, &rq->ready_list);
    task->on_rq = true;
    if (rq->curr == rq->idle)
        task_set_need_resched(rq->curr, true);
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
    quantum_ticks = time_ticks_from_ms(CONFIG_SCHED_QUANTUM_MS);
    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        struct rq *rq = cpu_rq(cpu_id);

        spin_lock_init(&rq->lock);
        rq->curr = NULL;
        rq->idle = NULL;
        rq->active_mm = NULL;
        rq->prev_mm = NULL;
        INIT_LIST_HEAD(&rq->ready_list);
        atomic_set(&rq->nr_runnable, 0);
        rq->quantum_deadline = 0;
    }
}

void sched_init_idle(unsigned int cpu_id, struct task *idle)
{
    struct rq *rq = cpu_rq(cpu_id);

    Assert(!rq->idle && idle->state == TASK_NEW && !idle->parent,
           "idle registration requires an unregistered NEW kernel task\n");
    task_get(idle);
    idle->cpu_id = cpu_id;
    idle->state = TASK_RUNNABLE;
    idle->on_rq = false;
    idle->on_cpu = false;
    task_set_need_resched(idle, false);
    rq->idle = idle;
    rq->curr = idle;
}

void sched_publish(struct task *task)
{
    struct rq *rq;
    unsigned int cpu_id;
    unsigned long placement_flags, rq_flags;
    bool notify_idle;

    Assert(task->state == TASK_NEW, "task already published\n");
    spin_lock_irqsave(&placement_lock, placement_flags);
    cpu_id = select_cpu();
    rq = cpu_rq(cpu_id);
    task->cpu_id = cpu_id;
    pid_publish(task);
    spin_lock_irqsave(&rq->lock, rq_flags);
    task_get(task);
    task->state = TASK_RUNNABLE;
    enqueue_task(rq, task);
    atomic_inc(&rq->nr_runnable);
    notify_idle = rq->curr == rq->idle && cpu_id != current_cpu_id();
    spin_unlock_irqrestore(&rq->lock, rq_flags);
    spin_unlock_irqrestore(&placement_lock, placement_flags);
    if (notify_idle)
        cpu_send_resched(cpu_id);
}

static bool wake_task(struct task *task, unsigned int states)
{
    struct rq *rq = task_rq(task);
    unsigned long flags;
    bool woke = false;
    bool notify_idle = false;

    spin_lock_irqsave(&rq->lock, flags);
    if (states & task->state) {
        task->state = TASK_RUNNABLE;
        if (!task->on_cpu) {
            enqueue_task(rq, task);
            atomic_inc(&rq->nr_runnable);
            notify_idle =
                rq->curr == rq->idle && task->cpu_id != current_cpu_id();
        }
        woke = true;
    }
    spin_unlock_irqrestore(&rq->lock, flags);
    if (notify_idle)
        cpu_send_resched(task->cpu_id);
    return woke;
}

bool sched_wake(struct task *task)
{
    return wake_task(task, TASK_NORMAL | TASK_KILLABLE);
}

bool sched_wake_interruptible(struct task *task)
{
    return wake_task(task, TASK_INTERRUPTIBLE);
}

bool sched_wake_signal(struct task *task, int sig)
{
    unsigned int states = TASK_INTERRUPTIBLE;

    if (sig == SIGKILL)
        states |= TASK_KILLABLE;
    return wake_task(task, states);
}

bool sched_resume(struct task *task)
{
    return wake_task(task, TASK_STOPPED);
}

void sched_stop(void)
{
    struct rq *rq = this_rq();
    unsigned long flags;

    might_sleep();
    preempt_disable();
    spin_lock_irqsave(&rq->lock, flags);
    if (task_test_flag(current, TF_STOPPENDING)) {
        task_clear_flag(current, TF_STOPPENDING);
        current->state = TASK_STOPPED;
    }
    spin_unlock_irqrestore(&rq->lock, flags);
    sched_park();
}

void sched_set_sleep_locked(struct rq *rq, unsigned int state)
{
    assert_spin_locked(&rq->lock);
    Assert(rq->curr != rq->idle &&
               (state == TASK_INTERRUPTIBLE || state == TASK_UNINTERRUPTIBLE ||
                state == TASK_KILLABLE),
           "only an ordinary current task can register sleep\n");
    rq->curr->state = state;
}

struct task *sched_prepare_switch_locked(struct rq *rq, bool rotate)
{
    struct task *prev = rq->curr;
    struct task *next;
    struct mm_struct *mm;

    assert_spin_locked(&rq->lock);
    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(local_irqs_disabled() && preempt_count() == 1,
                 "invalid scheduling context\n"));
    if (signal_pending_state(prev->state, prev))
        prev->state = TASK_RUNNABLE;
    if (prev->state != TASK_RUNNABLE) {
        atomic_dec(&rq->nr_runnable);
    } else {
        if (list_empty(&rq->ready_list)) {
            if (prev != rq->idle && (rotate || task_need_resched(prev)))
                rq->quantum_deadline = time_read() + quantum_ticks;
            task_set_need_resched(prev, false);
            return prev;
        }
        if (prev != rq->idle) {
            if (!rotate && !task_need_resched(prev))
                return prev;
            enqueue_task(rq, prev);
        }
    }

    task_set_need_resched(prev, false);
    next = pick_next_task(rq);
    mm = next->mm;
    if (mm != rq->active_mm) {
        if (mm)
            mm_get(mm);
        pgtable_switch(mm);
        rq->prev_mm = rq->active_mm;
        rq->active_mm = mm;
    }
    rq->curr = next;
    rq->quantum_deadline = time_read() + quantum_ticks;
    next->on_cpu = true;
    __spin_handoff(&rq->lock, next);
    return next;
}

struct mm_struct *sched_finish_switch_locked(struct rq *rq, struct task *prev)
{
    struct mm_struct *mm;

    assert_spin_locked(&rq->lock);
    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(local_irqs_disabled() && preempt_count() == 1,
                 "invalid switch finish context\n"));
    Assert(prev != rq->curr, "switch finish requires a real switch\n");
    prev->on_cpu = false;
    mm = rq->prev_mm;
    rq->prev_mm = NULL;
    return mm;
}

void sched_finish_switch(struct task *prev)
{
    struct rq *rq = this_rq();
    bool zombie = prev->state == TASK_ZOMBIE;
    struct mm_struct *mm __free(mm_put) = sched_finish_switch_locked(rq, prev);

    __spin_unlock(&rq->lock);
    if (zombie)
        task_finish_exit(prev);
}

__noreturn void sched_exit(void)
{
    struct rq *rq = this_rq();
    struct task *prev = current;
    struct task *next;

    local_irq_disable();
    __spin_lock(&rq->lock);
    Assert(prev != rq->idle, "idle cannot exit\n");
    Assert(!prev->mm && !prev->fdtable,
           "task resources must be detached before scheduler exit\n");
    prev->state = TASK_ZOMBIE;
    next = sched_prepare_switch_locked(rq, false);
    (void)switch_to(prev, next);
    panic("dead task resumed\n");
}

void sched_switch(bool rotate)
{
    unsigned long flags = local_irq_save();
    struct rq *rq = this_rq();
    struct task *prev;
    struct task *next;

    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(!cpus[current_cpu_id()].irq_depth,
                                       "scheduling in hard IRQ context\n"));
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

void preempt_schedule(void)
{
    do {
        preempt_disable();
        sched_switch(false);
    } while (task_need_resched(current));
}

void sched_park(void)
{
    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(preempt_count() == 1 && !local_irqs_disabled() &&
                     !cpus[current_cpu_id()].irq_depth &&
                     current != this_rq()->idle,
                 "invalid park context\n"));
    sched_switch(false);
    if (preemptible() && task_need_resched(current))
        preempt_schedule();
}

void sched_tick(uint64_t now)
{
    struct rq *rq = this_rq();

    __spin_lock(&rq->lock);
    if (rq->curr != rq->idle && now >= rq->quantum_deadline) {
        if (!list_empty(&rq->ready_list))
            task_set_need_resched(rq->curr, true);
        else
            rq->quantum_deadline = now + quantum_ticks;
    }
    __spin_unlock(&rq->lock);
}

void sched_irq_exit(void)
{
    if (preempt_count() || !task_need_resched(current))
        return;
    preempt_disable();
    sched_switch(false);
}

__noreturn void sched_task(void)
{
    struct rq *rq = this_rq();

    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(current == rq->idle && preempt_count() == INIT_PREEMPT_COUNT,
                 "invalid idle entry\n"));
    for (;;) {
        bool work;

        local_irq_disable();
        (void)cpu_handle_ipi();
        __spin_lock(&rq->lock);
        work = !list_empty(&rq->ready_list) || task_need_resched(rq->curr);
        __spin_unlock(&rq->lock);
        if (work) {
            sched_switch(false);
            preempt_disable();
            continue;
        }
        arch_cpu_idle();
        local_irq_enable();
    }
}

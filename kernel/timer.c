#include <nuvix/syscall.h>
#include <nuvix/timer.h>
#include <nuvix/module.h>
#include <nuvix/wait.h>
#include <nuvix/errno.h>
#include <nuvix/tick.h>
#include <nuvix/uaccess.h>
#include <asm/trap.h>

struct timers {
    struct wait_queue drain;
    struct rb_root root;
    struct timer *first;
    uint64_t next_sequence;
} __aligned(SMP_CACHE_BYTES);

static struct timers timers[CONFIG_NR_CPUS];

int timers_init(void)
{
    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        struct timers *base = &timers[cpu_id];

        wait_queue_init(&base->drain);
        base->root = RB_ROOT;
        base->first = NULL;
        base->next_sequence = 0;
    }
    return 0;
}
MODULE_INIT(timers_init, 50);

void timer_init(struct timer *timer, unsigned int cpu_id,
                void (*callback)(struct timer *timer))
{
    RB_CLEAR_NODE(&timer->node);
    timer->cpu_id = cpu_id;
    timer->callback = callback;
    timer->state = TIMER_IDLE;
}

int timer_start(struct timer *timer, uint64_t deadline)
{
    struct timers *base = &timers[timer->cpu_id];
    struct rb_node **link = &base->root.rb_node;
    struct rb_node *parent = NULL;
    unsigned long flags;
    bool first = true;

    spin_lock_irqsave(&base->drain.lock, flags);
    if (timer->state != TIMER_IDLE) {
        spin_unlock_irqrestore(&base->drain.lock, flags);
        return -EBUSY;
    }
    timer->deadline = deadline;
    timer->sequence = base->next_sequence++;
    while (*link) {
        struct timer *queued = rb_entry(*link, struct timer, node);

        parent = *link;
        if (deadline < queued->deadline ||
            (deadline == queued->deadline &&
             timer->sequence < queued->sequence)) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            first = false;
        }
    }
    rb_link_node(&timer->node, parent, link);
    rb_insert_color(&timer->node, &base->root);
    if (first)
        base->first = timer;
    timer->state = TIMER_QUEUED;
    spin_unlock_irqrestore(&base->drain.lock, flags);
    return 0;
}

static void remove_timer_locked(struct timers *base, struct timer *timer)
{
    if (base->first == timer)
        base->first = rb_entry_safe(rb_next(&timer->node), struct timer, node);
    rb_erase(&timer->node, &base->root);
    RB_CLEAR_NODE(&timer->node);
}

static int timer_cancel_locked(struct timers *base, struct timer *timer)
{
    if (timer->state == TIMER_QUEUED) {
        remove_timer_locked(base, timer);
        timer->state = TIMER_IDLE;
        return 1;
    }
    if (timer->state == TIMER_RUNNING || timer->state == TIMER_DRAINING) {
        timer->state = TIMER_DRAINING;
        return -EBUSY;
    }
    return 0;
}

int timer_cancel(struct timer *timer)
{
    struct timers *base = &timers[timer->cpu_id];
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&base->drain.lock, flags);
    ret = timer_cancel_locked(base, timer);
    spin_unlock_irqrestore(&base->drain.lock, flags);
    return ret;
}

void timer_cancel_sync(struct timer *timer)
{
    struct timers *base = &timers[timer->cpu_id];
    struct wait_entry wait;
    unsigned long flags;

    might_sleep();
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&base->drain.lock, flags);
        finish_wait_locked(&base->drain, &wait);
        if (timer_cancel_locked(base, timer) != -EBUSY) {
            spin_unlock_irqrestore(&base->drain.lock, flags);
            preempt_enable();
            return;
        }
        prepare_to_wait_locked(&base->drain, &wait, TASK_UNINTERRUPTIBLE);
        spin_unlock_irqrestore(&base->drain.lock, flags);
        sched_park();
    }
}

void timer_tick(uint64_t now)
{
    struct timers *base = &timers[current_cpu_id()];
    unsigned long flags;

    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(local_irqs_disabled() && cpus[current_cpu_id()].irq_depth,
                 "timer tick outside hard IRQ context\n"));
    for (;;) {
        struct timer *timer;
        bool drain;

        spin_lock_irqsave(&base->drain.lock, flags);
        timer = base->first;
        if (!timer || timer->deadline > now) {
            spin_unlock_irqrestore(&base->drain.lock, flags);
            return;
        }
        remove_timer_locked(base, timer);
        timer->state = TIMER_RUNNING;
        spin_unlock_irqrestore(&base->drain.lock, flags);

        timer->callback(timer);

        spin_lock_irqsave(&base->drain.lock, flags);
        drain = timer->state == TIMER_DRAINING;
        timer->state = TIMER_IDLE;
        /* No timer access after unlocking: a drained waiter may release it. */
        if (drain)
            wake_up_all_locked(&base->drain);
        spin_unlock_irqrestore(&base->drain.lock, flags);
    }
}

long sys_clock_gettime(struct trap_frame *frame)
{
    int clock_id = syscallarg(frame, 0);
    struct timespec64 *result = (struct timespec64 *)syscallarg(frame, 1);
    struct timespec64 ts;

    if (clock_id != CLOCK_MONOTONIC)
        return -EINVAL;
    time_get_monotonic(&ts);
    if (copy_to_user(result, &ts, sizeof(ts)))
        return -EFAULT;
    return 0;
}

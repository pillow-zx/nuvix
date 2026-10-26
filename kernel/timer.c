#include <nuvix/timer.h>
#include <nuvix/wait.h>
#include <nuvix/errno.h>
#include <nuvix/completion.h>
#include <nuvix/tick.h>

struct timer_base {
    /* drain.lock protects the tree, cached first node and execution state. */
    struct wait_queue drain;
    struct rb_root timers;
    struct timer *first;
    uint64_t next_sequence;
} __aligned(SMP_CACHE_BYTES);

static struct timer_base timer_bases[CONFIG_NR_CPUS];

void timers_init(void)
{
    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        struct timer_base *base = &timer_bases[cpu_id];

        wait_queue_init(&base->drain);
        base->timers = RB_ROOT;
        base->first = NULL;
        base->next_sequence = 0;
    }
}

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
    struct timer_base *base = &timer_bases[timer->cpu_id];
    struct rb_node **link = &base->timers.rb_node;
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
    rb_insert_color(&timer->node, &base->timers);
    if (first)
        base->first = timer;
    timer->state = TIMER_QUEUED;
    spin_unlock_irqrestore(&base->drain.lock, flags);
    return 0;
}

static void remove_timer_locked(struct timer_base *base, struct timer *timer)
{
    if (base->first == timer)
        base->first = rb_entry_safe(rb_next(&timer->node), struct timer, node);
    rb_erase(&timer->node, &base->timers);
    RB_CLEAR_NODE(&timer->node);
}

static int timer_cancel_locked(struct timer_base *base, struct timer *timer)
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
    struct timer_base *base = &timer_bases[timer->cpu_id];
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&base->drain.lock, flags);
    ret = timer_cancel_locked(base, timer);
    spin_unlock_irqrestore(&base->drain.lock, flags);
    return ret;
}

void timer_cancel_sync(struct timer *timer)
{
    struct timer_base *base = &timer_bases[timer->cpu_id];
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

struct timer_sleeper {
    struct timer timer;
    struct completion done;
};

static void timer_sleep_complete(struct timer *timer)
{
    struct timer_sleeper *sleeper =
        container_of(timer, struct timer_sleeper, timer);

    complete(&sleeper->done);
}

int timer_sleep_until(uint64_t deadline)
{
    struct timer_sleeper sleeper;
    int ret;

    might_sleep();
    if (deadline <= time_read())
        return 0;
    init_completion(&sleeper.done);
    timer_init(&sleeper.timer, current_cpu_id(), timer_sleep_complete);
    ret = timer_start(&sleeper.timer, deadline);
    if (ret)
        return ret;
    wait_for_completion(&sleeper.done);
    /* Completion may precede the timer core's final access to the stack. */
    timer_cancel_sync(&sleeper.timer);
    return 0;
}

int timer_sleep_ns(uint64_t ns)
{
    uint64_t deadline;
    int ret;

    might_sleep();
    if (!ns)
        return 0;
    ret = time_deadline_from_ns(ns, &deadline);
    if (ret)
        return ret;
    return timer_sleep_until(deadline);
}

void timer_tick(uint64_t now)
{
    struct timer_base *base = &timer_bases[current_cpu_id()];
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

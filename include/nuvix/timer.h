#ifndef _NUVIX_TIMER_H
#define _NUVIX_TIMER_H

#include <nuvix/rbtree.h>

enum timer_state {
    TIMER_IDLE,
    TIMER_QUEUED,
    TIMER_RUNNING,
    TIMER_DRAINING,
};

struct timer {
    struct rb_node node;
    /* Fixed at initialization; cpu_id selects the protecting timer base. */
    unsigned int cpu_id;
    void (*callback)(struct timer *timer);
    uint64_t deadline;
    uint64_t sequence;
    /* IDLE includes completed and cancelled timers. */
    enum timer_state state;
};

/* Boot-only, after CPU and clock setup, before starting SMP. */
void timers_init(void);
/* Exclusive initialization; cpu_id must be a valid online CPU when used. */
void timer_init(struct timer *timer, unsigned int cpu_id,
                void (*callback)(struct timer *timer));

/*
 * IRQ-safe one-shot registration with an absolute hardware-time deadline.
 * Returns 0 or -EBUSY if already queued/executing. Callbacks run on cpu_id in
 * hard IRQ context and must only perform short, non-sleeping notifications.
 * Keep timer and callback context alive through execution or synchronous cancel.
 */
int timer_start(struct timer *timer, uint64_t deadline);
/* IRQ-safe: 1 if removed, 0 if idle, -EBUSY if executing and still needs drain. */
int timer_cancel(struct timer *timer);
/*
 * Sleepable context, no caller locks; prevent concurrent restart. Returns with
 * no queued or executing callback, permitting reuse or release of its context.
 */
void timer_cancel_sync(struct timer *timer);

/*
 * Uninterruptible sleep in ordinary thread context, no caller locks or
 * preempt-disable levels. Requires clock, timers and the local CPU initialized.
 * Absolute deadlines use hardware counts; elapsed deadlines return 0 directly.
 * Returns 0 after expiry, with the private callback drained. Actual resumption
 * is subject to tick granularity and scheduling delay.
 */
int timer_sleep_until(uint64_t deadline);
/* Relative nanoseconds; zero returns 0 directly, overflow returns -EOVERFLOW. */
int timer_sleep_ns(uint64_t ns);

/* Local tick IRQ, with current bound and IRQs off. */
void timer_tick(uint64_t now);

#endif /* _NUVIX_TIMER_H */

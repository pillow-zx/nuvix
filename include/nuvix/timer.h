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

/* Local tick IRQ, with current bound and IRQs off. */
void timer_tick(uint64_t now);

#endif /* _NUVIX_TIMER_H */

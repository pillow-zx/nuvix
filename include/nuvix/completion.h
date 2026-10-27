#ifndef _NUVIX_COMPLETION_H
#define _NUVIX_COMPLETION_H

#include <nuvix/wait.h>

struct completion {
    /* Protected by wait.lock; UINT32_MAX means permanently completed. */
    uint32_t done;
    struct wait_queue wait;
};

#define COMPLETION_INITIALIZER(name)                                           \
    {.done = 0, .wait = WAIT_QUEUE_INITIALIZER((name).wait)}
#define DEFINE_COMPLETION(name)                                                \
    struct completion name = COMPLETION_INITIALIZER(name)

void init_completion(struct completion *completion);
/* Exclusive reuse after all prior waiters and completion operations finish. */
void reinit_completion(struct completion *completion);

/* IRQ-safe notifications; keep the object alive until the operation returns. */
void complete(struct completion *completion);
void complete_all(struct completion *completion);

void wait_for_completion(struct completion *completion);
int wait_for_completion_interruptible(struct completion *completion);
int wait_for_completion_killable(struct completion *completion);

/*
 * Uninterruptible wait with an absolute hardware-time deadline. Ordinary
 * sleepable context, no caller locks or preempt-disable levels; timers ready.
 * Returns 0 after consuming a completion (unless complete_all), or -ETIMEDOUT.
 * Available completion takes precedence over expiry at each locked decision.
 * Returns with the private timer drained; timeout does not cancel the producer.
 * Keep completion and producer context alive until all their users finish.
 */
int wait_for_completion_until(struct completion *completion, uint64_t deadline);
/* IRQ-safe, nonblocking consumption; false if no completion is available. */
bool try_wait_for_completion(struct completion *completion);

#endif /* _NUVIX_COMPLETION_H */

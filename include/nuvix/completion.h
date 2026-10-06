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

/* Ordinary sleepable context; consumes one completion unless complete_all. */
void wait_for_completion(struct completion *completion);
/* IRQ-safe, nonblocking consumption; false if no completion is available. */
bool try_wait_for_completion(struct completion *completion);

#endif /* _NUVIX_COMPLETION_H */

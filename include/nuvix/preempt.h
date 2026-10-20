#ifndef _NUVIX_PREEMPT_H
#define _NUVIX_PREEMPT_H

#include <nuvix/irq.h>

#define INIT_PREEMPT_COUNT 1U

void preempt_schedule(void);

unsigned int preempt_count(void);

void preempt_disable(void);

void preempt_enable_no_resched(void);

bool preemptible(void);

void preempt_enable(void);

#endif /* _NUVIX_PREEMPT_H */

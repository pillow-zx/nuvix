#ifndef _NUVIX_TRACE_H
#define _NUVIX_TRACE_H

#include <nuvix/ctypes.h>

#ifdef CONFIG_TRACE_CPURING

#define TRACE_RING_SIZE 256U

enum trace_type {
    TRACE_SWITCH,
    TRACE_WAKE,
    TRACE_IRQ_ENTER,
    TRACE_IRQ_LEAVE,
};

struct trace_event {
    uint64_t timestamp;
    uintptr_t task;
    enum trace_type type;
    union {
        struct {
            uintptr_t next;
            unsigned int pstate;
        } switch_task;
        struct {
            uintptr_t target;
            unsigned int cpu;
            unsigned int ostate;
        } wake;
        unsigned int irq;
    } data;
};

struct trace_ring {
    unsigned int next;
    unsigned int used;
    struct trace_event events[TRACE_RING_SIZE];
};

struct task;

void trace_switch(const struct task *next, unsigned int prev_state);
void trace_wake(const struct task *target, unsigned int old_state);
void trace_irq_enter(unsigned int irq);
void trace_irq_leave(unsigned int irq);

void trace_dump(void);

#endif /* CONFIG_TRACE_CPURING */

#endif /* _NUVIX_TRACE_H */

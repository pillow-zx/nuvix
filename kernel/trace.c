#include <nuvix/trace.h>

#ifdef CONFIG_TRACE_CPURING
#include <nuvix/cpu.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>
#include <nuvix/tick.h>

static void trace_record(struct trace_event *event)
{
    unsigned long flags = local_irq_save();
    struct trace_ring *ring = &this_cpu()->ring;

    event->timestamp = time_read();
    event->task = (uintptr_t)current;
    ring->events[ring->next] = *event;
    barrier();
    ring->next = (ring->next + 1) % TRACE_RING_SIZE;
    if (ring->used < TRACE_RING_SIZE)
        ++ring->used;
    local_irq_restore(flags);
}

void trace_switch(const struct task *next, unsigned int prev_state)
{
    struct trace_event event = {
        .type = TRACE_SWITCH,
        .data.switch_task.next = (uintptr_t)next,
        .data.switch_task.pstate = prev_state,
    };

    trace_record(&event);
}

void trace_wake(const struct task *target, unsigned int old_state)
{
    struct trace_event event = {
        .type = TRACE_WAKE,
        .data.wake.target = (uintptr_t)target,
        .data.wake.cpu = target->cpu_id,
        .data.wake.ostate = old_state,
    };

    trace_record(&event);
}

void trace_irq_enter(unsigned int irq)
{
    struct trace_event event = {
        .type = TRACE_IRQ_ENTER,
        .data.irq = irq,
    };

    trace_record(&event);
}

void trace_irq_leave(unsigned int irq)
{
    struct trace_event event = {
        .type = TRACE_IRQ_LEAVE,
        .data.irq = irq,
    };

    trace_record(&event);
}

void trace_dump(void)
{
    unsigned long flags = local_irq_save();
    struct trace_ring *ring = &this_cpu()->ring;
    unsigned int start =
        (ring->next + TRACE_RING_SIZE - ring->used) % TRACE_RING_SIZE;

    printk("[trace] cpu=%u events=%u timestamp=ticks\n", current_cpu_id(),
           ring->used);
    for (unsigned int i = 0; i < ring->used; ++i) {
        const struct trace_event *event =
            &ring->events[(start + i) % TRACE_RING_SIZE];
        unsigned long long tick = (unsigned long long)event->timestamp;

        switch (event->type) {
        case TRACE_SWITCH:
            printk("  tick=%llu switch prev=%p next=%p prev_state=%#x\n", tick,
                   (void *)event->task, (void *)event->data.switch_task.next,
                   event->data.switch_task.pstate);
            break;
        case TRACE_WAKE:
            printk("  tick=%llu wake current=%p target=%p "
                   "target_cpu=%u old_state=%#x\n",
                   tick, (void *)event->task, (void *)event->data.wake.target,
                   event->data.wake.cpu, event->data.wake.ostate);
            break;
        case TRACE_IRQ_ENTER:
            printk("  tick=%llu irq_enter task=%p irq=%u\n", tick,
                   (void *)event->task, event->data.irq);
            break;
        case TRACE_IRQ_LEAVE:
            printk("  tick=%llu irq_leave task=%p irq=%u\n", tick,
                   (void *)event->task, event->data.irq);
            break;
        }
    }
    local_irq_restore(flags);
}
#endif /* CONFIG_TRACE_CPURING */


#include <nuvix/irq.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/trace.h>

struct irq_action {
    irq_handler_t handler;
    void *data;
};

static struct irq_action *irq_actions;
static unsigned int nr_sources;

int irq_init(void)
{
    struct irq_action *actions __free(kfree) = NULL;
    unsigned int nr_irqs;
    int ret;

    ret = arch_irq_init(&nr_irqs);
    if (ret)
        return ret;
    actions = kzalloc(((size_t)nr_irqs + 1) * sizeof(*actions));
    if (!actions)
        return -ENOMEM;
    irq_actions = move(actions);
    nr_sources = nr_irqs;
    return 0;
}

int irq_parse(int node, unsigned int *irq)
{
    unsigned int source;
    int ret = arch_irq_parse(node, &source);

    if (ret)
        return ret;
    if (!source || source > nr_sources)
        return -EINVAL;
    *irq = source;
    return 0;
}

int irq_register(unsigned int irq, irq_handler_t handler, void *data)
{
    struct irq_action *action;

    if (!irq || irq > nr_sources || !handler)
        return -EINVAL;
    action = &irq_actions[irq];
    if (action->handler)
        return -EEXIST;
    action->data = data;
    action->handler = handler;
    return 0;
}

int irq_unregister(unsigned int irq)
{
    struct irq_action *action;
    int ret;

    if (!irq || irq > nr_sources)
        return -EINVAL;
    Assert(local_irqs_disabled(), "IRQ unregister requires local IRQs off\n");
    action = &irq_actions[irq];
    if (!action->handler)
        return -ENOENT;
    ret = arch_irq_disable(irq);
    if (ret)
        return ret;
    action->handler = NULL;
    action->data = NULL;
    return 0;
}

int irq_enable(unsigned int irq)
{
    if (!irq || irq > nr_sources)
        return -EINVAL;
    return arch_irq_enable(irq);
}

int irq_disable(unsigned int irq)
{
    if (!irq || irq > nr_sources)
        return -EINVAL;
    return arch_irq_disable(irq);
}

void irq_dispatch(unsigned int irq)
{
    Assert(irq && irq <= nr_sources, "invalid interrupt source %u\n", irq);
    struct irq_action *action = &irq_actions[irq];

    Assert(action->handler, "unhandled interrupt source %u\n", irq);
    IFDEF(CONFIG_DEBUG_TRACE_CPURING, trace_irq_enter(irq));
    action->handler(action->data);
    IFDEF(CONFIG_DEBUG_TRACE_CPURING, trace_irq_leave(irq));
}

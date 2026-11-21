#include <nuvix/irq.h>
#include <nuvix/printk.h>

int arch_irq_init(unsigned int *nr_irqs)
{
    int ret = plic_init();

    if (ret)
        return ret;
    *nr_irqs = plic_nr_irqs();
    return 0;
}

int arch_irq_parse(int node, unsigned int *irq)
{
    return plic_irq_parse(node, irq);
}

int arch_irq_enable(unsigned int irq)
{
    return plic_enable(irq);
}

int arch_irq_disable(unsigned int irq)
{
    return plic_disable(irq);
}

void arch_handle_external_irq(void)
{
    unsigned int nr_irqs = plic_nr_irqs();
    unsigned int irq;

    while ((irq = plic_claim())) {
        if (irq > nr_irqs) {
            pr_warn("PLIC: invalid interrupt source %u\n", irq);
            return;
        }
        irq_dispatch(irq);
        plic_complete(irq);
    }
}

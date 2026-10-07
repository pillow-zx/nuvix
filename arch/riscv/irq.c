#include <nuvix/irq.h>

int arch_irq_init(const struct dtb *dtb, unsigned int *nr_irqs)
{
    int ret = plic_init(dtb);

    if (ret)
        return ret;
    *nr_irqs = plic_nr_irqs();
    return 0;
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
    unsigned int irq;

    while ((irq = plic_claim())) {
        irq_dispatch(irq);
        plic_complete(irq);
    }
}

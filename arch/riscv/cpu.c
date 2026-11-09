#include <nuvix/cpu.h>

extern char __alltraps[];

void arch_cpu_init(void)
{
    arch_local_irq_disable();
    csr_clear(sstatus, SSTATUS_FS | SSTATUS_SUM | SSTATUS_MXR);
    csr_write(sscratch, 0);
#ifdef CONFIG_SMP
    arch_clear_ipi();
#endif
    csr_write(stvec, (unsigned long)__alltraps);
    csr_write(sie, SIE_STIE);
#ifdef CONFIG_SMP
    csr_set(sie, SIE_SSIE);
#endif
    /* only allow cpu 0 to handle external irq */
    if (current_cpu_id() == 0)
        csr_set(sie, SIE_SEIE);
}

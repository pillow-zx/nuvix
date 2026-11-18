#ifndef _ASM_RISCV_CPU_H
#define _ASM_RISCV_CPU_H

#include <nuvix/compiler.h>
#include <asm/riscv/irq.h>
#include <asm/riscv/barrier.h>

#define SMP_CACHE_BYTES 64

#ifdef CONFIG_SMP
long arch_send_ipi(unsigned int cpu_id);
struct task;
long arch_start_cpu(unsigned int cpu_id, struct task *idle);

static __always_inline void arch_clear_ipi(void)
{
    csr_clear(sip, SIP_SSIP);
}

#endif /* CONFIG_SMP */

void arch_cpu_init(void);

#define wfi()                                                                  \
    do {                                                                       \
        __asm__ volatile("wfi" ::: "memory");                                  \
    } while (0)

/* Global IRQs are off; enabled sources can still wake WFI on pending IRQs. */
static __always_inline void arch_cpu_idle(void)
{
    io_mb();
    wfi();
}

/* Permanently park this hart; this is not the normal idle path. */
static __always_inline __noreturn void arch_halt(void)
{
    arch_local_irq_disable();
    for (;;)
        wfi();
}

#endif /* _ASM_RISCV_CPU_H */

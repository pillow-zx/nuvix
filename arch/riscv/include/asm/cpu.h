#ifndef _NUVIX_RISCV_CPU_H
#define _NUVIX_RISCV_CPU_H

#include <nuvix/compiler.h>
#include <asm/irq.h>
#include <asm/barrier.h>

#define SMP_CACHE_BYTES 64

long arch_send_ipi(unsigned int cpu_id);
struct task;
long arch_start_cpu(unsigned int cpu_id, struct task *idle);
void arch_cpu_init(void);

static __always_inline void arch_clear_ipi(void)
{
    csr_clear(sip, SIP_SSIP);
}

#define wfi()                                                                  \
    do {                                                                       \
        __asm__ volatile("wfi" ::: "memory");                                  \
    } while (0)

/* Global IRQs are off; SSIE/STIE remain enabled to wake WFI on pending IRQs. */
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

#endif /* _NUVIX_RISCV_CPU_H */

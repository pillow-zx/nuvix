#ifndef _NUVIX_RISCV_CPU_H
#define _NUVIX_RISCV_CPU_H

#include <nuvix/compiler.h>
#include <asm/irq.h>

#define SMP_CACHE_BYTES 64

long arch_send_ipi(unsigned int cpu_id);

#define wfi()                                                                  \
    do {                                                                       \
        __asm__ volatile("wfi" ::: "memory");                                  \
    } while (0)

/* Permanently park this hart; this is not the normal idle path. */
static __always_inline __noreturn void arch_halt(void)
{
    arch_local_irq_disable();
    for (;;)
        wfi();
}

#endif /* _NUVIX_RISCV_CPU_H */

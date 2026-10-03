#ifndef _NUVIX_RISCV_CPU_H
#define _NUVIX_RISCV_CPU_H

#include <nuvix/compiler.h>
#include <nuvix/arch/riscv/irq.h>

#define wfi()                                                                  \
    do {                                                                       \
        __asm__ volatile("wfi" ::: "memory");                                  \
    } while (0)

/* Permanently park this hart; this is not the normal idle path. */
static __always_inline __noreturn inline void arch_halt(void)
{
    arch_local_irq_disable();
    for (;;)
        wfi();
}

#endif /* _NUVIX_RISCV_CPU_H */

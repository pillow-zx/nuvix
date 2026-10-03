#ifndef _NUVIX_RISCV_CPU_H
#define _NUVIX_RISCV_CPU_H

#include <nuvix/compiler.h>
#include <nuvix/arch/riscv/csr.h>

#define wfi()                                                                  \
    do {                                                                       \
        __asm__ volatile("wfi" ::: "memory");                                  \
    } while (0)

/* Permanently park this hart; this is not the normal idle path. */
static __always_inline __noreturn inline void arch_halt(void)
{
    csr_clear(sstatus, SSTATUS_SIE);
    for (;;)
        wfi();
}

#endif /* _NUVIX_RISCV_CPU_H */

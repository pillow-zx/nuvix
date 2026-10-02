#ifndef _NUVIX_RISCV_SBI_H
#define _NUVIX_RISCV_SBI_H

#include <nuvix/compiler.h>

#define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x01
#define SBI_EXT_0_1_CONSOLE_GETCHAR 0x02

static __always_inline inline long sbi_legacy_console_call(unsigned long eid, unsigned long arg)
{
    register unsigned long a0 __asm__("a0") = arg;
    register unsigned long a7 __asm__("a7") = eid;

    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (long)a0;
}

static inline int sbi_putchar(int c)
{
    long ret =
        sbi_legacy_console_call(SBI_EXT_0_1_CONSOLE_PUTCHAR, (unsigned char)c);

    return ret < 0 ? (int)ret : (unsigned char)c;
}

static inline int sbi_getchar(void)
{
    return (int)sbi_legacy_console_call(SBI_EXT_0_1_CONSOLE_GETCHAR, 0);
}

#endif /* _NUVIX_RISCV_SBI_H */

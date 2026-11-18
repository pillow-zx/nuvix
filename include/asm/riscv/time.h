#ifndef _ASM_RISCV_TIME_H
#define _ASM_RISCV_TIME_H

#include <asm/riscv/csr.h>
#include <asm/riscv/sbi.h>
#include <nuvix/ctypes.h>

static __always_inline uint64_t arch_read_time(void)
{
    return csr_read(time);
}

static inline long arch_set_timer(uint64_t deadline)
{
    return sbi_set_timer(deadline).error;
}

#endif /* _ASM_RISCV_TIME_H */

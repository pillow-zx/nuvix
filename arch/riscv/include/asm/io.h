#ifndef _NUVIX_RISCV_IO_H
#define _NUVIX_RISCV_IO_H

#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <asm/barrier.h>

static __always_inline uint8_t readb(const volatile void *addr)
{
    uint8_t value;

    io_mb();
    value = *(const volatile uint8_t *)addr;
    io_mb();
    return value;
}

static __always_inline void writeb(uint8_t value, volatile void *addr)
{
    io_mb();
    *(volatile uint8_t *)addr = value;
    io_mb();
}

#endif /* _NUVIX_RISCV_IO_H */

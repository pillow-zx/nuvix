#ifndef _NUVIX_RISCV_BARRIER_H
#define _NUVIX_RISCV_BARRIER_H

#define io_mb()                                                                \
    do {                                                                       \
        __asm__ volatile("fence iorw, iorw" ::: "memory");                     \
    } while (0)

#endif /* _NUVIX_RISCV_BARRIER_H */

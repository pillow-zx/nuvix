#ifndef _NUVIX_RISCV_BARRIER_H
#define _NUVIX_RISCV_BARRIER_H

#define io_mb()                                                                \
    do {                                                                       \
        __asm__ volatile("fence iorw, iorw" ::: "memory");                     \
    } while (0)

/* Conservative device-visible ordering for coherent DMA memory. */
#define arch_dma_wmb() io_mb()
#define arch_dma_rmb() io_mb()

#endif /* _NUVIX_RISCV_BARRIER_H */

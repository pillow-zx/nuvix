#ifndef _NUVIX_DMA_H
#define _NUVIX_DMA_H

#include <nuvix/ctypes.h>
#include <asm/barrier.h>
#include <asm/dma.h>

typedef uint64_t dma_t;

enum dma_direction {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL,
};

struct page;

/*
 * Current support is limited to coherent RAM on QEMU virt. Requires
 * buddy_init(); allocation does not sleep or reclaim. dma_addr must be non-NULL.
 * Returns a zeroed, directly mapped block of PAGE_SIZE << order bytes and its
 * device address. Failure returns NULL without changing *dma_addr.
 */
void *dma_alloc_coherent(int order, dma_t *dma_addr);
/*
 * Pass the original CPU address and order after all device access has ended.
 * NULL is a no-op. This does not stop the device or wait for completion.
 */
void dma_free_coherent(void *cpu_addr, int order);

/*
 * page belongs to a live page allocation; offset/length describe a nonempty
 * region within that page. The caller guarantees the range and keeps the
 * allocation alive until device completion; no page reference is acquired.
 * dma_addr must be non-NULL. Returns 0, or -EINVAL for an invalid direction
 * without changing *dma_addr. Mapping does not publish a request or add barriers.
 */
int dma_map_page(const struct page *page, size_t offset, size_t length,
                 enum dma_direction direction, dma_t *dma_addr);
/*
 * After device completion, pass the address, length and direction from map.
 * No-op on the current coherent direct-DMA platform; does not free the page.
 */
void dma_unmap_page(dma_t dma_addr, size_t length,
                    enum dma_direction direction);

#define dma_wmb() arch_dma_wmb()
#define dma_rmb() arch_dma_rmb()

#endif /* _NUVIX_DMA_H */

#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/page.h>
#include <nuvix/string.h>
#include <asm/dma.h>

void *dma_alloc_coherent(int order, dma_t *dma_addr)
{
    struct page *page = alloc_pages(order);
    paddr_t paddr;
    void *cpu_addr;

    if (!page)
        return NULL;
    paddr = page_to_phys(page);
    cpu_addr = (void *)__va(paddr);
    memset(cpu_addr, 0, (size_t)PAGE_SIZE << order);
    *dma_addr = arch_phys_to_dma(paddr);
    return cpu_addr;
}

void dma_free_coherent(void *cpu_addr, int order)
{
    if (!cpu_addr)
        return;
    free_pages(phys_to_page(__pa((uintptr_t)cpu_addr)), order);
}

int dma_map_page(const struct page *page, size_t offset, size_t length,
                 enum dma_direction direction, dma_t *dma_addr)
{
    if (direction != DMA_TO_DEVICE && direction != DMA_FROM_DEVICE &&
        direction != DMA_BIDIRECTIONAL)
        return -EINVAL;
    (void)length;
    *dma_addr = arch_phys_to_dma(page_to_phys(page) + offset);
    return 0;
}

void dma_unmap_page(dma_t dma_addr, size_t length,
                    enum dma_direction direction)
{
    (void)dma_addr;
    (void)length;
    (void)direction;
}

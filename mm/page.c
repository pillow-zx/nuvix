#include <nuvix/page.h>
#include <nuvix/memblock.h>
#include <nuvix/errno.h>
#include <nuvix/compiler.h>

struct page *mem_map;
unsigned long min_pfn, max_pfn;

int page_init(uint64_t start, uint64_t end)
{
    struct page *pages;
    unsigned long first_pfn, last_pfn;
    uint64_t phys_addr;
    size_t nr_pages, bytes;
    int ret;

    if (mem_map)
        return -EBUSY;

    /* Round the start up without overflowing the physical address. */
    first_pfn = memblock.memory.base >> PAGE_SHIFT;
    if (memblock.memory.base & (PAGE_SIZE - 1))
        ++first_pfn;
    last_pfn = (memblock.memory.base + memblock.memory.size) >> PAGE_SHIFT;
    if (first_pfn >= last_pfn)
        return -ENODEV;
    nr_pages = last_pfn - first_pfn;
    if (check_mul_overflow(nr_pages, sizeof(*pages), &bytes))
        return -EOVERFLOW;

    ret = early_alloc(bytes, PAGE_SIZE, start, end, &phys_addr);
    if (ret)
        return ret;
    pages = (struct page *)(uintptr_t)phys_addr;

    for (size_t i = 0; i < nr_pages; ++i) {
        pages[i] = (struct page){
            .flags = PG_RESERVED,
            .refcount = REFCOUNT_INIT(0),
            .order = PAGE_ORDER_NONE,
        };
    }

    min_pfn = first_pfn;
    max_pfn = last_pfn;
    mem_map = pages;
    return 0;
}

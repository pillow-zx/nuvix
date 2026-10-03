#ifndef _NUVIX_PAGE_H
#define _NUVIX_PAGE_H

#include <nuvix/bitops.h>
#include <nuvix/layout.h>
#include <nuvix/list.h>
#include <nuvix/refcount.h>

#define PG_RESERVED     BIT(0)
#define PG_BUDDY        BIT(1)
#define PAGE_ORDER_NONE ((unsigned int)-1)

struct page {
    unsigned long flags;
    refcount_t refcount;
    /* Order and list membership apply only to a valid block head. */
    unsigned int order;
    struct list_head node;
};

/* The flat array describes every complete RAM page in [min_pfn, max_pfn). */
extern struct page *mem_map;
extern unsigned long min_pfn, max_pfn;

/*
 * Boot-only, after memblock_init(). [start, end) must be identity-mapped RAM
 * accessible for metadata allocation. All descriptors start reserved, with
 * zero references, no order and NULL list links. Buddy must consult the final
 * memblock reservations before releasing any pages.
 * The array pointer must be rebound before removing the identity mapping.
 */
int page_init(uint64_t start, uint64_t end);

static inline bool pfn_valid(unsigned long pfn)
{
    return pfn >= min_pfn && pfn < max_pfn;
}

/* Reserved pages have descriptors too; invalid PFNs return NULL. */
static inline struct page *pfn_to_page(unsigned long pfn)
{
    if (!pfn_valid(pfn))
        return NULL;
    return mem_map + (pfn - min_pfn);
}

/* The caller must pass a descriptor belonging to mem_map. */
static inline unsigned long page_to_pfn(const struct page *page)
{
    return min_pfn + (unsigned long)(page - mem_map);
}

/* Returns the descriptor for the containing physical page, or NULL. */
static inline struct page *phys_to_page(uint64_t phys_addr)
{
    return pfn_to_page(phys_addr >> PAGE_SHIFT);
}

/* Returns a physical address, not a kernel virtual address. */
static inline uint64_t page_to_phys(const struct page *page)
{
    return (uint64_t)page_to_pfn(page) << PAGE_SHIFT;
}

#endif /* _NUVIX_PAGE_H */

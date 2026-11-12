#ifndef _NUVIX_PAGE_H
#define _NUVIX_PAGE_H

#include <nuvix/bitops.h>
#include <nuvix/layout.h>
#include <nuvix/list.h>
#include <nuvix/refcount.h>
#include <nuvix/cleanup.h>

#define PG_RESERVED     BIT(0)
#define PG_BUDDY        BIT(1)
#define PG_PGTABLE      BIT(2)
#define PG_ALLOCATED    BIT(3)
#define PG_SLAB         BIT(4)
#define PG_KMALLOC      BIT(5)
#define PAGE_ORDER_NONE ((unsigned int)-1)

struct kmem_cache;

struct page {
    unsigned long flags;
    refcount_t refcount;
    /* Order and list membership apply only to a valid block head. */
    unsigned int order;
    struct list_head node;
    struct {
        struct kmem_cache *cache;
        void *freelist;
        unsigned int inuse;
    } slab;
};

/* The flat array describes every complete RAM page in [min_pfn, max_pfn). */
extern struct page *mem_map;
extern unsigned long min_pfn, max_pfn;

/*
 * Boot-only, with MMU off, after memblock_init(). All descriptors start
 * reserved, with zero references, no order and NULL list links. Buddy must
 * consult the final memblock reservations before releasing any pages.
 * Rebind the array pointer to its kernel virtual address before the MMU switch.
 */
int page_init(void);

/*
 * Requires buddy_init(). Only the block head owns a reference;
 * interior pages must not be referenced.
 * Orders are in [0, CONFIG_MAX_ORDER); allocation does not zero memory.
 */
struct page *alloc_pages(int order);
/*
 * Drop one reference using the original head and order. NULL is a no-op;
 * invalid frees are diagnosed and leave allocator state unchanged.
 */
void free_pages(struct page *page, int order);

static inline struct page *alloc_page(void)
{
    return alloc_pages(0);
}

static inline void free_page(struct page *page)
{
    free_pages(page, 0);
}

DECLARE_FREE(page, struct page *, if (_T) free_page(_T))
DECLARE_FREE(kstack, struct page *, if (_T) free_pages(_T, KSTACK_ORDER));

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
static inline struct page *phys_to_page(uint64_t paddr)
{
    return pfn_to_page(paddr >> PAGE_SHIFT);
}

/* Returns a physical address, not a kernel virtual address. */
static inline uint64_t page_to_phys(const struct page *page)
{
    return (uint64_t)page_to_pfn(page) << PAGE_SHIFT;
}

#endif /* _NUVIX_PAGE_H */

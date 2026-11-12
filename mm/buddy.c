#define pr_fmt(fmt) "buddy: " fmt

#include <nuvix/page.h>
#include <nuvix/memblock.h>
#include <nuvix/module.h>
#include <nuvix/spinlock.h>
#include <nuvix/printk.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>

static struct free_area {
    struct list_head list;
    unsigned long nr_free;
} free_area[CONFIG_MAX_ORDER];

static DEFINE_SPINLOCK(buddy_lock);
static unsigned long managed_pages;

static void add_block(struct page *page, unsigned int order)
{
    page->flags |= PG_BUDDY;
    page->order = order;
    list_add(&page->node, &free_area[order].list);
    ++free_area[order].nr_free;
}

static void del_block(struct page *page)
{
    --free_area[page->order].nr_free;
    list_del(&page->node);
    page->flags &= ~PG_BUDDY;
    page->order = PAGE_ORDER_NONE;
}

static void merge_block(unsigned long pfn, unsigned int order)
{
    while (order + 1 < CONFIG_MAX_ORDER) {
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        struct page *buddy = pfn_to_page(buddy_pfn);

        if (!buddy || !(buddy->flags & PG_BUDDY) || buddy->order != order)
            break;
        del_block(buddy);
        pfn = MIN(pfn, buddy_pfn);
        ++order;
    }
    add_block(pfn_to_page(pfn), order);
}

static void release_range(unsigned long start, unsigned long end)
{
    for (unsigned long pfn = start; pfn < end; ++pfn)
        pfn_to_page(pfn)->flags &= ~PG_RESERVED;
    managed_pages += end - start;

    while (start < end) {
        unsigned int order = CONFIG_MAX_ORDER - 1;

        while ((start & ((1UL << order) - 1)) || (1UL << order) > end - start)
            --order;
        add_block(pfn_to_page(start), order);
        start += 1UL << order;
    }
}

static unsigned long snapshot(unsigned long *counts)
{
    unsigned long total = 0;
    unsigned long flags;

    spin_lock_irqsave(&buddy_lock, flags);
    for (unsigned int order = 0; order < CONFIG_MAX_ORDER; ++order) {
        counts[order] = free_area[order].nr_free;
        total += counts[order] << order;
    }
    spin_unlock_irqrestore(&buddy_lock, flags);
    return total;
}

static void buddy_dump(void)
{
    unsigned long counts[CONFIG_MAX_ORDER];
    unsigned long total = snapshot(counts);

    pr_info(
        "managed=%u reserved=%u free=%u pages\n", (unsigned int)managed_pages,
        (unsigned int)(max_pfn - min_pfn - managed_pages), (unsigned int)total);
    for (unsigned int order = 0; order < CONFIG_MAX_ORDER; ++order)
        pr_info("order=%u blocks=%u pages=%u\n", order,
                (unsigned int)counts[order],
                (unsigned int)(counts[order] << order));
}

struct page *alloc_pages(int order)
{
    struct page *page = NULL;
    unsigned long flags;

    if (order < 0 || order >= CONFIG_MAX_ORDER)
        return NULL;

    spin_lock_irqsave(&buddy_lock, flags);
    for (unsigned int split_order = order; split_order < CONFIG_MAX_ORDER;
         ++split_order) {
        if (list_empty(&free_area[split_order].list))
            continue;
        page =
            list_first_entry(&free_area[split_order].list, struct page, node);
        del_block(page);
        while (split_order > (unsigned int)order) {
            --split_order;
            add_block(page + (1UL << split_order), split_order);
        }
        page->flags |= PG_ALLOCATED;
        page->order = order;
        refcount_set(&page->refcount, 1);
        break;
    }
    spin_unlock_irqrestore(&buddy_lock, flags);
    return page;
}

static bool allocated_block(struct page *page, int order)
{
    uintptr_t addr = (uintptr_t)page;
    uintptr_t start = (uintptr_t)mem_map;
    unsigned long pfn, nr;

    if (order < 0 || order >= CONFIG_MAX_ORDER)
        return false;
    if (addr < start || addr >= (uintptr_t)(mem_map + (max_pfn - min_pfn)) ||
        (addr - start) % sizeof(*page))
        return false;

    pfn = page_to_pfn(page);
    nr = 1UL << order;
    if ((pfn & (nr - 1)) || nr > max_pfn - pfn || page->flags != PG_ALLOCATED ||
        page->order != (unsigned int)order || !refcount_read(&page->refcount))
        return false;
    for (unsigned long i = 1; i < nr; ++i) {
        if (page[i].flags || page[i].order != PAGE_ORDER_NONE ||
            refcount_read(&page[i].refcount))
            return false;
    }
    return true;
}

void free_pages(struct page *page, int order)
{
    unsigned long flags;

    if (!page)
        return;

    spin_lock_irqsave(&buddy_lock, flags);
    if (!allocated_block(page, order)) {
        spin_unlock_irqrestore(&buddy_lock, flags);
        pr_warn("reject free page=%p order=%d\n", (void *)page, order);
        return;
    }
    if (refcount_dec_and_test(&page->refcount)) {
        page->flags &= ~PG_ALLOCATED;
        page->order = PAGE_ORDER_NONE;
        merge_block(page_to_pfn(page), order);
    }
    spin_unlock_irqrestore(&buddy_lock, flags);
}

static int buddy_init(void)
{
    unsigned long start = min_pfn;

    for (unsigned int order = 0; order < CONFIG_MAX_ORDER; ++order)
        INIT_LIST_HEAD(&free_area[order].list);

    /* Reservations are sorted by base but can overlap and be unaligned. */
    for (unsigned int i = 0; i < memblock.nr_reserved; ++i) {
        const struct memblock_region *region = &memblock.reserved[i];
        paddr_t end = region->base + region->size;
        unsigned long first = region->base >> PAGE_SHIFT;
        unsigned long last = (end >> PAGE_SHIFT) + !!(end & (PAGE_SIZE - 1));

        if (last <= start)
            continue;
        if (first >= max_pfn)
            break;
        if (start < first)
            release_range(start, first);
        start = MIN(last, max_pfn);
    }
    release_range(start, max_pfn);

    buddy_dump();
    return 0;
}
MODULE_INIT(buddy_init, 10);

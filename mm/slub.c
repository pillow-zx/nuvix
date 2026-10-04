#define pr_fmt(fmt) "slub: " fmt

#include <nuvix/slab.h>
#include <nuvix/page.h>
#include <nuvix/spinlock.h>
#include <nuvix/string.h>
#include <nuvix/printk.h>
#include <nuvix/assert.h>

#define KMALLOC_SHIFT_LOW  3
#define KMALLOC_SHIFT_HIGH 12
#define KMALLOC_MAX_SIZE   (1UL << KMALLOC_SHIFT_HIGH)

struct kmem_cache {
    unsigned int size;
    unsigned int objects;
    spinlock_t lock;
    struct list_head partial;
    struct list_head full;
};

static struct kmem_cache caches[KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1];

static_assert((1UL << KMALLOC_SHIFT_LOW) >= sizeof(void *),
              "smallest object must hold a free pointer");
static_assert(KMALLOC_MAX_SIZE <= PAGE_SIZE,
              "kmalloc caches must fit in a single page");

static struct page *new_slab(struct kmem_cache *cache)
{
    struct page *page = alloc_page();
    unsigned char *base;

    if (!page)
        return NULL;
    base = (void *)__va(page_to_phys(page));
    page->flags |= PG_SLAB;
    page->slab.cache = cache;
    page->slab.freelist = base;
    page->slab.inuse = 0;
    for (unsigned int i = 0; i < cache->objects; ++i) {
        void *object = base + i * cache->size;

        *(void **)object =
            i + 1 < cache->objects ? base + (i + 1) * cache->size : NULL;
    }
    return page;
}

static void *cache_alloc(struct kmem_cache *cache)
{
    struct page *page;
    void *object;

    spin_lock(&cache->lock);
    if (list_empty(&cache->partial)) {
        spin_unlock(&cache->lock);
        page = new_slab(cache);
        if (!page)
            return NULL;
        spin_lock(&cache->lock);
        list_add(&page->node, &cache->partial);
    }
    page = list_first_entry(&cache->partial, struct page, node);
    object = page->slab.freelist;
    page->slab.freelist = *(void **)object;
    ++page->slab.inuse;
    if (!page->slab.freelist)
        list_move_tail(&page->node, &cache->full);
    spin_unlock(&cache->lock);
    return object;
}

void *kmalloc(size_t size)
{
    unsigned int index = 0;
    unsigned int order = 0;
    size_t bytes = PAGE_SIZE;
    struct page *page;

    if (!size)
        return NULL;
    if (size <= KMALLOC_MAX_SIZE) {
        while (size > (1UL << (KMALLOC_SHIFT_LOW + index)))
            ++index;
        return cache_alloc(&caches[index]);
    }
    while (bytes < size) {
        if (++order >= CONFIG_MAX_ORDER)
            return NULL;
        bytes <<= 1;
    }
    page = alloc_pages(order);
    if (!page)
        return NULL;
    page->flags |= PG_KMALLOC;
    return (void *)__va(page_to_phys(page));
}

void *kzalloc(size_t size)
{
    void *ptr = kmalloc(size);

    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void kfree(const void *ptr)
{
    struct kmem_cache *cache;
    struct page *page;
    bool empty;

    if (!ptr)
        return;
    page = phys_to_page(__pa((uintptr_t)ptr));
    if (!page || !(page->flags & (PG_SLAB | PG_KMALLOC))) {
        pr_warn("reject free object=%p\n", (void *)ptr);
        return;
    }
    if (page->flags & PG_KMALLOC) {
        unsigned int order = page->order;

        if ((uintptr_t)ptr != __va(page_to_phys(page))) {
            pr_warn("reject large free object=%p\n", (void *)ptr);
            return;
        }
        page->flags &= ~PG_KMALLOC;
        free_pages(page, order);
        return;
    }

    cache = page->slab.cache;
    spin_lock(&cache->lock);
    if (page->slab.inuse == cache->objects)
        list_move_tail(&page->node, &cache->partial);
    *(void **)ptr = page->slab.freelist;
    page->slab.freelist = (void *)ptr;
    --page->slab.inuse;
    empty = !page->slab.inuse;
    if (empty)
        list_del(&page->node);
    spin_unlock(&cache->lock);

    if (empty) {
        page->flags &= ~PG_SLAB;
        page->slab.cache = NULL;
        page->slab.freelist = NULL;
        free_page(page);
    }
}

void slub_init(void)
{
    for (unsigned int i = 0; i < sizeof(caches) / sizeof(caches[0]); ++i) {
        struct kmem_cache *cache = &caches[i];

        cache->size = 1U << (KMALLOC_SHIFT_LOW + i);
        cache->objects = PAGE_SIZE / cache->size;
        spin_lock_init(&cache->lock);
        INIT_LIST_HEAD(&cache->partial);
        INIT_LIST_HEAD(&cache->full);
    }
}

#include <nuvix/assert.h>
#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/hashtable.h>
#include <nuvix/mapping.h>
#include <nuvix/mutex.h>
#include <nuvix/page.h>
#include <nuvix/sched.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/wait.h>

#define MAPPING_HASH_BITS  8
#define MAPPING_DIRTY_HIGH (MAPPING_CACHE_PAGES * 3 / 4)

struct mapping_cache {
    struct hlist_head table[BIT(MAPPING_HASH_BITS)];
    struct list_head dirty;
    struct list_head lru;
    struct mutex lock;
    struct mutex wlock;
    struct wait_queue flush_wait;
    unsigned int nr;
    unsigned int nr_dirty;
    int error;
};

static struct mapping_cache cache;

static uint64_t mapping_key(const struct mapping *m, uint64_t index)
{
    return (uint64_t)(uintptr_t)m ^ index;
}

static struct cache_page *mapping_find(struct mapping *m, uint64_t index)
{
    struct cache_page *cp;

    hash_for_each_possible (cache.table, cp, hash, mapping_key(m, index)) {
        if (cp->mapping == m && cp->index == index)
            return cp;
    }
    return NULL;
}

static void cache_page_destroy(struct cache_page *cp)
{
    free_page(cp->page);
    kfree(cp);
}

static void mapping_link(struct cache_page *cp)
{
    list_del_init(&cp->list);
    if (!hash_hashed(&cp->hash))
        return;
    if (cp->state & CACHE_PAGE_DIRTY)
        list_add_tail(&cp->list, &cache.dirty);
    else if (refcount_read(&cp->refs) == 1)
        list_add_tail(&cp->list, &cache.lru);
}

static bool mapping_make_room(void)
{
    struct cache_page *victim;

    if (cache.nr < MAPPING_CACHE_PAGES)
        return true;
    if (list_empty(&cache.lru))
        return false;

    victim = list_first_entry(&cache.lru, struct cache_page, list);
    Assert(refcount_read(&victim->refs) == 1 && !victim->state,
           "busy page on the mapping LRU\n");
    list_del_init(&victim->list);
    hash_del(&victim->hash);
    cache.nr--;
    if (refcount_dec_and_test(&victim->refs))
        cache_page_destroy(victim);
    return true;
}

struct cache_page *mapping_get(struct mapping *m, uint64_t index)
{
    struct cache_page *cp;
    struct cache_page *cached;
    struct page *page;
    int ret;

    might_sleep();
    mutex_lock(&cache.lock);
    cp = mapping_find(m, index);
    if (cp) {
        refcount_inc(&cp->refs);
        mapping_link(cp);
        mutex_unlock(&cache.lock);
        return cp;
    }
    mutex_unlock(&cache.lock);

    page = alloc_page();
    if (!page)
        return ERR_PTR(-ENOMEM);
    cp = kzalloc(sizeof(*cp));
    if (!cp) {
        free_page(page);
        return ERR_PTR(-ENOMEM);
    }
    cp->page = page;
    cp->mapping = m;
    cp->index = index;
    refcount_set(&cp->refs, 1);
    INIT_LIST_HEAD(&cp->list);

    ret = m->m_ops->read_page(cp);
    if (ret)
        goto fail;

    mutex_lock(&cache.lock);
    cached = mapping_find(m, index);
    if (cached) {
        refcount_inc(&cached->refs);
        mapping_link(cached);
        mutex_unlock(&cache.lock);
        cache_page_destroy(cp);
        return cached;
    }
    if (!mapping_make_room()) {
        mutex_unlock(&cache.lock);
        wake_up_one(&cache.flush_wait);
        ret = -ENOMEM;
        goto fail;
    }
    refcount_inc(&cp->refs);
    hash_add(cache.table, &cp->hash, mapping_key(m, index));
    cache.nr++;
    mutex_unlock(&cache.lock);
    return cp;

fail:
    free_page(page);
    kfree(cp);
    return ERR_PTR(ret);
}

void *mapping_data(const struct cache_page *cp)
{
    return (void *)__va(page_to_phys(cp->page));
}

void mapping_dirty(struct cache_page *cp)
{
    bool wake = false;

    mutex_lock(&cache.lock);
    if (!hash_hashed(&cp->hash)) {
        mutex_unlock(&cache.lock);
        return;
    }
    if (!(cp->state & CACHE_PAGE_DIRTY)) {
        cp->state |= CACHE_PAGE_DIRTY;
        if (++cache.nr_dirty >= MAPPING_DIRTY_HIGH)
            wake = true;
    }
    mapping_link(cp);
    mutex_unlock(&cache.lock);

    if (wake)
        wake_up_one(&cache.flush_wait);
}

void mapping_put(struct cache_page *cp)
{
    mutex_lock(&cache.lock);
    Assert(refcount_read(&cp->refs), "mapping_put without a reference\n");
    if (refcount_dec_and_test(&cp->refs)) {
        Assert(!hash_hashed(&cp->hash),
               "dropping the mapping cache reference with mapping_put\n");
        mutex_unlock(&cache.lock);
        cache_page_destroy(cp);
        return;
    }
    mapping_link(cp);
    mutex_unlock(&cache.lock);
}

int mapping_writeback(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    int ret;

    might_sleep();
    mutex_lock(&cache.wlock);
    mutex_lock(&cache.lock);
    if (!(cp->state & CACHE_PAGE_DIRTY)) {
        mutex_unlock(&cache.lock);
        mutex_unlock(&cache.wlock);
        return 0;
    }
    cp->state &= ~CACHE_PAGE_DIRTY;
    cp->state |= CACHE_PAGE_WRITEBACK;
    cache.nr_dirty--;
    refcount_inc(&cp->refs);
    mapping_link(cp);
    mutex_unlock(&cache.lock);

    ret = m->m_ops->write_page(cp);

    mutex_lock(&cache.lock);
    cp->state &= ~CACHE_PAGE_WRITEBACK;
    if (ret)
        cache.error = ret;
    refcount_dec(&cp->refs);
    mapping_link(cp);
    mutex_unlock(&cache.lock);
    mutex_unlock(&cache.wlock);
    return ret;
}

static struct cache_page *mapping_first_dirty(struct mapping *m)
{
    struct cache_page *cp;

    list_for_each_entry (cp, &cache.dirty, list) {
        if (cp->mapping == m)
            return cp;
    }
    return NULL;
}

int mapping_flush(struct mapping *m)
{
    unsigned int budget = MAPPING_CACHE_PAGES + 1;
    int ret = cache.error;

    might_sleep();
    while (budget--) {
        struct cache_page *cp;
        int err;

        mutex_lock(&cache.lock);
        cp = mapping_first_dirty(m);
        if (cp)
            refcount_inc(&cp->refs);
        mutex_unlock(&cache.lock);
        if (!cp)
            break;

        err = mapping_writeback(cp);
        mapping_put(cp);
        if (err && !ret)
            ret = err;
    }

    mutex_lock(&cache.wlock);
    {
        int err = blk_flush(m->m_bdev);

        if (err && !ret)
            ret = err;
    }
    mutex_unlock(&cache.wlock);
    return ret;
}

void mapping_purge(struct mapping *m, uint64_t start, uint64_t end)
{
    struct hlist_node *tmp;
    struct cache_page *cp;
    unsigned int bucket;

    mutex_lock(&cache.wlock);
    mutex_lock(&cache.lock);
    hash_for_each_safe (cache.table, bucket, tmp, cp, hash) {
        if (cp->mapping != m || cp->index < start || cp->index >= end)
            continue;
        Assert(!(cp->state & CACHE_PAGE_WRITEBACK),
               "purging page %llu during writeback\n",
               (unsigned long long)cp->index);
        if (cp->state & CACHE_PAGE_DIRTY) {
            cp->state &= ~CACHE_PAGE_DIRTY;
            cache.nr_dirty--;
        }
        hash_del(&cp->hash);
        list_del_init(&cp->list);
        cache.nr--;
        if (refcount_dec_and_test(&cp->refs))
            cache_page_destroy(cp);
    }
    mutex_unlock(&cache.lock);
    mutex_unlock(&cache.wlock);
}

static int mapping_flusher(void *arg)
{
    (void)arg;

    for (;;) {
        wait_event(&cache.flush_wait, !list_empty(&cache.dirty));

        for (;;) {
            struct cache_page *cp;

            mutex_lock(&cache.lock);
            cp = list_empty(&cache.dirty)
                     ? NULL
                     : list_first_entry(&cache.dirty, struct cache_page, list);
            if (cp)
                refcount_inc(&cp->refs);
            mutex_unlock(&cache.lock);
            if (!cp)
                break;

            (void)mapping_writeback(cp);
            mapping_put(cp);
        }
    }
    return 0;
}

int mapping_init(void)
{
    struct task *flusher __free(task_put) = NULL;

    hash_init(cache.table);
    INIT_LIST_HEAD(&cache.dirty);
    INIT_LIST_HEAD(&cache.lru);
    mutex_init(&cache.lock);
    mutex_init(&cache.wlock);
    wait_queue_init(&cache.flush_wait);

    flusher = task_alloc(false);
    if (!flusher)
        return -ENOMEM;
    arch_ktask_init(&flusher->ctx, mapping_flusher, NULL);
    sched_publish(flusher);
    return 0;
}

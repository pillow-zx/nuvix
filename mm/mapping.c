#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/hashtable.h>
#include <nuvix/mapping.h>
#include <nuvix/module.h>
#include <nuvix/page.h>
#include <nuvix/sched.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/wait.h>

#define MAPPING_HASH_BITS 8

struct mapping_cache {
    struct hlist_head table[BIT(MAPPING_HASH_BITS)];
    struct list_head dirty;
    struct list_head lru;
    struct mutex mutex;
    struct wait_queue wait;
    unsigned int nr;
};

static struct mapping_cache cache;

static void mapping_assert_locked(struct mapping *m)
{
    if (m->mutex)
        assert_mutex_locked(m->mutex);
}

static void mapping_hold(struct mapping *m)
{
    if (m->m_ops->get)
        m->m_ops->get(m);
}

static void mapping_release(struct mapping *m)
{
    if (m->m_ops->put)
        m->m_ops->put(m);
}

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
    unsigned long flags;

    spin_lock_irqsave(&cache.wait.lock, flags);
    list_del_init(&cp->list);
    if (hash_hashed(&cp->hash)) {
        if (cp->state)
            list_add_tail(&cp->list, &cache.dirty);
        else if (refcount_read(&cp->refs) == 1)
            list_add_tail(&cp->list, &cache.lru);
    }
    spin_unlock_irqrestore(&cache.wait.lock, flags);
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
    mapping_assert_locked(m);
    mutex_lock(&cache.mutex);
    cp = mapping_find(m, index);
    if (cp) {
        refcount_inc(&cp->refs);
        mapping_link(cp);
        mutex_unlock(&cache.mutex);
        goto ready;
    }
    mutex_unlock(&cache.mutex);

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
    mutex_init(&cp->mutex);
    mutex_lock(&cp->mutex);

    mutex_lock(&cache.mutex);
    cached = mapping_find(m, index);
    if (cached) {
        refcount_inc(&cached->refs);
        mapping_link(cached);
        mutex_unlock(&cache.mutex);
        mutex_unlock(&cp->mutex);
        cache_page_destroy(cp);
        cp = cached;
        goto ready;
    }
    if (!mapping_make_room()) {
        mutex_unlock(&cache.mutex);
        mutex_unlock(&cp->mutex);
        cache_page_destroy(cp);
        wake_up_one(&cache.wait);
        return ERR_PTR(-ENOMEM);
    }
    refcount_inc(&cp->refs);
    hash_add(cache.table, &cp->hash, mapping_key(m, index));
    cache.nr++;
    mutex_unlock(&cache.mutex);

    ret = m->m_ops->read_page(cp);
    cp->error = ret;
    if (ret) {
        mutex_lock(&cache.mutex);
        hash_del(&cp->hash);
        cache.nr--;
        refcount_dec(&cp->refs);
        mutex_unlock(&cache.mutex);
    }
    mutex_unlock(&cp->mutex);
    if (ret) {
        mapping_put(cp);
        return ERR_PTR(ret);
    }
    return cp;

ready:
    mutex_lock(&cp->mutex);
    ret = cp->error;
    mutex_unlock(&cp->mutex);
    if (ret) {
        mapping_put(cp);
        return ERR_PTR(ret);
    }
    return cp;
}

void *mapping_data(const struct cache_page *cp)
{
    assert_mutex_locked(&cp->mutex);
    return (void *)__va(page_to_phys(cp->page));
}

void mapping_dirty(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;

    mapping_assert_locked(m);
    assert_mutex_locked(&cp->mutex);
    Assert(m->m_ops->write_page, "dirty page without a write backend\n");
    mutex_lock(&cache.mutex);
    Assert(hash_hashed(&cp->hash), "dirtying a detached cache page\n");
    if (!(cp->state & CACHE_PAGE_DIRTY)) {
        mapping_hold(m);
        cp->state |= CACHE_PAGE_DIRTY;
    }
    mapping_link(cp);
    mutex_unlock(&cache.mutex);
}

void mapping_put(struct cache_page *cp)
{
    mutex_lock(&cache.mutex);
    Assert(refcount_read(&cp->refs), "mapping_put without a reference\n");
    if (refcount_dec_and_test(&cp->refs)) {
        Assert(!hash_hashed(&cp->hash),
               "dropping the mapping cache reference with mapping_put\n");
        mutex_unlock(&cache.mutex);
        cache_page_destroy(cp);
        return;
    }
    mapping_link(cp);
    mutex_unlock(&cache.mutex);
}

int mapping_writeback(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    int ret;

    might_sleep();
    mapping_assert_locked(m);
    mutex_lock(&cp->mutex);
    mutex_lock(&cache.mutex);
    if (!(cp->state & CACHE_PAGE_DIRTY)) {
        mutex_unlock(&cache.mutex);
        mutex_unlock(&cp->mutex);
        return 0;
    }
    cp->state = CACHE_PAGE_WRITEBACK;
    refcount_inc(&cp->refs);
    mapping_link(cp);
    mutex_unlock(&cache.mutex);

    ret = m->m_ops->write_page(cp);

    mutex_lock(&cache.mutex);
    cp->state = 0;
    if (ret)
        m->m_error = ret;
    refcount_dec(&cp->refs);
    mapping_link(cp);
    mutex_unlock(&cache.mutex);
    mutex_unlock(&cp->mutex);
    mapping_release(m);
    return ret;
}

static unsigned int mapping_collect(struct mapping *m,
                                    struct cache_page **pages)
{
    struct cache_page *cp;
    unsigned int nr = 0;

    mutex_lock(&cache.mutex);
    list_for_each_entry (cp, &cache.dirty, list) {
        if (m && cp->mapping != m)
            continue;
        mapping_hold(cp->mapping);
        refcount_inc(&cp->refs);
        pages[nr++] = cp;
    }
    mutex_unlock(&cache.mutex);
    return nr;
}

int mapping_flush(struct mapping *m)
{
    struct cache_page **pages __free(kfree) = NULL;
    unsigned int nr;
    int ret;
    int err;

    might_sleep();
    mapping_assert_locked(m);
    pages = kmalloc(MAPPING_CACHE_PAGES * sizeof(*pages));
    if (!pages)
        return -ENOMEM;
    nr = mapping_collect(m, pages);
    mutex_lock(&cache.mutex);
    ret = m->m_error;
    mutex_unlock(&cache.mutex);
    for (unsigned int i = 0; i < nr; i++) {
        struct cache_page *cp = pages[i];

        err = mapping_writeback(cp);
        mapping_put(cp);
        mapping_release(m);
        if (err && !ret)
            ret = err;
    }

    return ret;
}

void mapping_purge(struct mapping *m, uint64_t start, uint64_t end)
{
    mapping_assert_locked(m);
    for (;;) {
        struct cache_page *cp;
        struct cache_page *victim = NULL;
        unsigned long flags;
        unsigned int bucket;
        bool dirty = false;

        mutex_lock(&cache.mutex);
        hash_for_each (cache.table, bucket, cp, hash) {
            if (cp->mapping == m && cp->index >= start && cp->index < end) {
                victim = cp;
                refcount_inc(&cp->refs);
                break;
            }
        }
        mutex_unlock(&cache.mutex);
        if (!victim)
            break;

        mutex_lock(&victim->mutex);
        mutex_lock(&cache.mutex);
        if (hash_hashed(&victim->hash)) {
            Assert(!(victim->state & CACHE_PAGE_WRITEBACK),
                   "purging page during writeback\n");
            dirty = victim->state & CACHE_PAGE_DIRTY;
            victim->state = 0;
            hash_del(&victim->hash);
            spin_lock_irqsave(&cache.wait.lock, flags);
            list_del_init(&victim->list);
            spin_unlock_irqrestore(&cache.wait.lock, flags);
            cache.nr--;
            refcount_dec(&victim->refs);
        }
        mutex_unlock(&cache.mutex);
        mutex_unlock(&victim->mutex);
        mapping_put(victim);
        if (dirty)
            mapping_release(m);
    }
}

static int mapping_flusher(void *arg)
{
    static struct cache_page *pages[MAPPING_CACHE_PAGES];

    (void)arg;
    for (;;) {
        unsigned int nr;

        wait_event(&cache.wait, !list_empty(&cache.dirty));
        nr = mapping_collect(NULL, pages);
        for (unsigned int i = 0; i < nr; i++) {
            struct cache_page *cp = pages[i];
            struct mapping *m = cp->mapping;

            if (m->mutex)
                mutex_lock(m->mutex);
            (void)mapping_writeback(cp);
            if (m->mutex)
                mutex_unlock(m->mutex);
            mapping_put(cp);
            mapping_release(m);
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
    mutex_init(&cache.mutex);
    wait_queue_init(&cache.wait);

    flusher = task_alloc(false);
    if (!flusher)
        return -ENOMEM;
    arch_ktask_init(&flusher->ctx, mapping_flusher, NULL);
    sched_publish(flusher);
    return 0;
}
MODULE_INIT(mapping_init, 100);

#ifndef _NUVIX_MAPPING_H
#define _NUVIX_MAPPING_H

#include <nuvix/bitops.h>
#include <nuvix/cleanup.h>
#include <nuvix/ctypes.h>
#include <nuvix/hlist.h>
#include <nuvix/list.h>
#include <nuvix/refcount.h>

#define MAPPING_CACHE_PAGES 256

struct blkdev;
struct cache_page;
struct mapping;
struct page;

struct mapping_ops {
    int (*read_page)(struct cache_page *cp);
    int (*write_page)(struct cache_page *cp);
};

struct mapping {
    struct blkdev *m_bdev;
    const struct mapping_ops *m_ops;
    void *m_host;
};

#define CACHE_PAGE_DIRTY     BIT(0)
#define CACHE_PAGE_WRITEBACK BIT(1)

struct cache_page {
    struct hlist_node hash;
    struct list_head list;
    struct page *page;
    struct mapping *mapping;
    uint64_t index;
    refcount_t refs;
    unsigned int state;
};

int mapping_init(void);

void *mapping_data(const struct cache_page *cp);

struct cache_page *mapping_get(struct mapping *m, uint64_t index);

void mapping_dirty(struct cache_page *cp);

void mapping_put(struct cache_page *cp);

int mapping_writeback(struct cache_page *cp);

int mapping_flush(struct mapping *m);

void mapping_purge(struct mapping *m, uint64_t start, uint64_t end);

DECLARE_FREE(cache_page, struct cache_page *, if (_T) mapping_put(_T))

#endif /* ifndef _NUVIX_MAPPING_H */

#ifndef _NUVIX_BLOCK_PAGE_CACHE_INTERNAL_H
#define _NUVIX_BLOCK_PAGE_CACHE_INTERNAL_H

#include <nuvix/page_cache.h>
#include <nuvix/hlist.h>
#include <nuvix/spinlock.h>

struct pgcache {
	dev_t dev;
	uint64_t block;
	uint8_t *data;
	uint32_t refcount;
	bool uptodate;
	bool dirty;
	bool writeback;
	bool filling;
	bool dropped;
	int error;
	uint64_t dirty_generation;
	struct wait_channel waitq;
	struct hlist_node hash_node;
	struct list_head lru_node;
	struct list_head dirty_node;
	struct list_head associations;
};

struct pgcache_assoc {
	struct page_mapping *mapping;
	uint64_t index;
	struct pgcache *page;
	struct hlist_node hash_node;
	struct list_head page_node;
	struct list_head mapping_node;
};

/* References and in-flight I/O block a mapping change.  Eviction uses its
 * own stricter predicate. */
static inline bool pgcache_mapping_change_locked(const struct pgcache *page)
{
	return page->refcount != 0 || page->writeback || page->filling;
}

void pgcache_init(void);

struct pgcache *pgcache_find(dev_t dev, uint64_t block);

struct pgcache *pgcache_find_mapping(struct page_mapping *mapping, uint64_t index);

void pgcache_clear_dirty(struct pgcache *page);

void pgcache_clear_dirty_locked(struct pgcache *page);

struct pgcache *pgcache_dirty_any(void);

struct pgcache_assoc *pgcache_assoc_find_locked(struct page_mapping *mapping, uint64_t index);

void pgcache_assoc_remove_locked(struct pgcache_assoc *assoc, struct list_head *removed);

void pgcache_assoc_remove_mapping_locked(struct page_mapping *mapping, struct list_head *removed);

void pgcache_assoc_remove_page_locked(struct pgcache *page, struct list_head *removed);

void pgcache_assoc_free_list(struct list_head *removed);

bool pgcache_assoc_has_page_locked(struct pgcache *page);

int page_cache_assoc_add(struct page_mapping *mapping, uint64_t index, struct pgcache *page);

extern spinlock_t pgcache_lock;

extern struct list_head pgcache_dirty_list;

#endif

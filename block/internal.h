#ifndef _CUTEOS_BLOCK_PAGE_CACHE_INTERNAL_H
#define _CUTEOS_BLOCK_PAGE_CACHE_INTERNAL_H

#include <kernel/page_cache.h>
#include <kernel/spinlock.h>

struct pgcache {
	dev_t dev;
	uint64_t block;
	uint8_t *data;
	uint32_t refcount;
	bool uptodate;
	bool dirty;
	bool writeback;
	bool dropped;
	struct list_head hash_node;
	struct list_head lru_node;
	struct list_head dirty_node;
};

struct pgcache_assoc {
	struct page_mapping *mapping;
	uint64_t index;
	struct pgcache *page;
	struct list_head page_node;
	struct list_head mapping_node;
};

void pgcache_init(void);
void pgcache_wb_init(void);
struct pgcache *pgcache_find(dev_t dev, uint64_t block);
struct pgcache *pgcache_find_mapping(struct page_mapping *mapping,
					   uint64_t index);
void pgcache_clear_dirty(struct pgcache *page);
void pgcache_clear_dirty_locked(struct pgcache *page);
struct pgcache *pgcache_dirty_any(void);
struct pgcache *pgcache_dirty_any_locked(void);
int pgcache_wb_run(struct pgcache *start, struct page_mapping *mapping);
void pgcache_assoc_remove_mapping(struct page_mapping *mapping);
void pgcache_assoc_remove_page_locked(struct pgcache *page,
					 struct list_head *removed);
void pgcache_assoc_free_list(struct list_head *removed);
bool pgcache_assoc_has_page_locked(struct pgcache *page);
int page_cache_assoc_add(struct page_mapping *mapping, uint64_t index,
			 struct pgcache *page);
extern spinlock_t pgcache_lock;
extern struct list_head pgcache_associations;
extern struct list_head pgcache_dirty_list;

#endif

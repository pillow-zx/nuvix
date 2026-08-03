#ifndef _CUTEOS_BLOCK_PAGE_CACHE_INTERNAL_H
#define _CUTEOS_BLOCK_PAGE_CACHE_INTERNAL_H

#include <kernel/page_cache.h>
#include <kernel/spinlock.h>

struct page_cache {
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

struct page_cache_assoc {
	struct page_mapping *mapping;
	uint64_t index;
	struct page_cache *page;
	struct list_head page_node;
	struct list_head mapping_node;
};

void page_cache_init(void);
void page_cache_wb_init(void);
struct page_cache *page_cache_find(dev_t dev, uint64_t block);
struct page_cache *page_cache_find_mapping(struct page_mapping *mapping,
					   uint64_t index);
void page_cache_clear_dirty(struct page_cache *page);
void page_cache_clear_dirty_locked(struct page_cache *page);
struct page_cache *page_cache_dirty_any(void);
struct page_cache *page_cache_dirty_any_locked(void);
int page_cache_wb_run(struct page_cache *start, struct page_mapping *mapping);
void page_cache_assoc_remove_mapping(struct page_mapping *mapping);
void page_cache_assoc_remove_page_locked(struct page_cache *page,
					 struct list_head *removed);
void page_cache_assoc_free_list(struct list_head *removed);
bool page_cache_assoc_has_page_locked(struct page_cache *page);
int page_cache_assoc_add(struct page_mapping *mapping, uint64_t index,
			 struct page_cache *page);
extern spinlock_t pgcache_lock;
extern struct list_head pgcache_associations;
extern struct list_head pgcache_dirty_list;

#endif

#ifndef _NUVIX_PAGE_CACHE_H
#define _NUVIX_PAGE_CACHE_H

/** @file page_cache.h @brief Physical 4 KiB page-cache interface. */

#include <nuvix/compiler.h>
#include <nuvix/page_mapping.h>
#include <nuvix/bitops.h>
#include <nuvix/cleanup.h>
#include <nuvix/types.h>
#include <nuvix/vfs.h>

#define PAGE_CACHE_READ         BIT(0)
#define PAGE_CACHE_CREATE       BIT(1)

struct pgcache;

__must_check
struct pgcache *pgcache_get(dev_t dev, uint64_t block, uint32_t flags, int *error);

__must_check
struct pgcache *pgcache_get_mapping(struct page_mapping *mapping, uint64_t index, uint32_t flags, int *error);

__must_check
struct pgcache *pgcache_get_block(dev_t dev, uint64_t block);

__must_check
struct pgcache *pgcache_get_data(void *data);

void pgcache_put_page(struct pgcache *page);

__must_check __pure
uint8_t *page_cache_data(struct pgcache *page);

__must_check
bool pgcache_is_uptodate(const struct pgcache *page);

void pgcache_set_uptodate(struct pgcache *page, bool uptodate);

__must_check
bool pgcache_is_dirty(const struct pgcache *page);

void pgcache_mark_dirty(struct pgcache *page);

/* Shared-mapping and mutation lifecycle helpers. */
__must_check
int pgcache_shared_write_begin(struct pgcache *page);
void pgcache_shared_write_end(struct pgcache *page);
__must_check
bool pgcache_has_shared_writers(const struct pgcache *page);
__must_check
int pgcache_mutation_begin(struct pgcache *page);
void pgcache_mutation_end(struct pgcache *page, bool dirty);

__must_check
int pgcache_sync_page(struct pgcache *page);

__must_check
int pgcache_sync_mapping(struct page_mapping *mapping);

__must_check
int pgcache_msync_mapping_range(struct page_mapping *mapping,
				uint64_t first_page, uint64_t end_page);

__must_check
int pgcache_sync_inode(struct inode *inode);

__must_check
int pgcache_sync_all(void);
__must_check
int pgcache_sync_device(dev_t dev);

__must_check
int pgcache_truncate_mapping(struct page_mapping *mapping, uint64_t size);

__must_check
int pgcache_invalidate_mapping(struct page_mapping *mapping);

__must_check
int pgcache_truncate_inode(struct inode *inode, uint64_t size);

__must_check
int pgcache_invalidate_inode(struct inode *inode);

__must_check
int pgcache_discard_device(dev_t dev);

void pgcache_wb_thread(void *arg);

CLEANUP_DEFINE(pgcache_put, struct pgcache *, if (_T) pgcache_put_page(_T))

#endif

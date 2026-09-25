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
#define PAGE_CACHE_NOWAIT       BIT(2)

struct pgcache;

/* These operations never wait for device I/O. -EAGAIN means an in-flight
 * producer will notify pgcache_progress_channel(), then the caller retries.
 * Cache exhaustion without such a producer returns -ENOMEM. */
struct wait_channel *pgcache_progress_channel(void);

void pgcache_signal_progress(void);

void pgcache_async_thread(void *arg);

int pgcache_try_read_block(dev_t dev, uint64_t block, struct pgcache **out);

int pgcache_try_create_block(dev_t dev, uint64_t block, struct pgcache **out);

int pgcache_try_get_mapping(struct page_mapping *mapping, uint64_t index,
			    uint32_t flags, struct pgcache **out);

int pgcache_sync_mapping_step(struct page_mapping *mapping);

int pgcache_sync_device_step(dev_t dev);

__must_check
struct pgcache *pgcache_get(dev_t dev, uint64_t block, uint32_t flags, int *error);

__must_check
struct pgcache *pgcache_get_mapping(struct page_mapping *mapping, uint64_t index, uint32_t flags, int *error);

__must_check
struct pgcache *pgcache_get_block(dev_t dev, uint64_t block);

void pgcache_put_page(struct pgcache *page);

__must_check __pure
uint8_t *page_cache_data(struct pgcache *page);

__must_check
bool pgcache_is_uptodate(const struct pgcache *page);

void pgcache_set_uptodate(struct pgcache *page, bool uptodate);

__must_check
bool pgcache_is_dirty(const struct pgcache *page);

void pgcache_mark_dirty(struct pgcache *page);

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

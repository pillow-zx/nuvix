/* Physical page cache keyed by (device, block). */
#include "internal.h"

#include <nuvix/blkdev.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/hashtable.h>
#include <nuvix/list.h>
#include <nuvix/slab.h>

#define PGCACHE_HASH_BITS	7
#define PGCACHE_NR_PAGES	512U

HASH_TABLE_DECLARE_STATIC(pgcache_hashtable, PGCACHE_HASH_BITS);
LIST_HEAD_STATIC(pgcache_lru);
LIST_HEAD(pgcache_dirty_list);
LIST_HEAD(pgcache_associations);
DEFINE_SPINLOCK(pgcache_lock, LOCK_RANK_PAGE_CACHE, LOCK_IRQ_TASK_ONLY);
static uint32_t page_cache_pages;
static bool page_cache_ready;

static uint32_t pgcache_hash(dev_t dev, uint64_t block)
{
	return (uint32_t)(dev ^ block ^ (block >> PGCACHE_HASH_BITS));
}

void pgcache_init(void)
{
	if (page_cache_ready)
		return;
	hash_table_init(&pgcache_hashtable);
	INIT_LIST_HEAD(&pgcache_lru);
	INIT_LIST_HEAD(&pgcache_dirty_list);
	INIT_LIST_HEAD(&pgcache_associations);
	pgcache_wb_init();
	page_cache_ready = true;
}

struct pgcache *pgcache_find(dev_t dev, uint64_t block)
{
	struct hlist_node *pos;
	uint32_t hash = pgcache_hash(dev, block);

	hash_table_for_each_possible (pos, &pgcache_hashtable, hash) {
		struct pgcache *page =
			hlist_entry(pos, struct pgcache, hash_node);
		if (page->dev == dev && page->block == block)
			return page;
	}
	return NULL;
}

struct pgcache *pgcache_get_data(void *data)
{
	struct hlist_node *pos;
	struct pgcache *page = NULL;
	irq_flags_t flags;

	if (!data)
		return NULL;
	spin_lock_irqsave(&pgcache_lock, &flags);
	for (uint32_t bucket = 0;
	     bucket < HASH_TABLE_SIZE(pgcache_hashtable.bits) && !page;
	     bucket++) {
		hlist_for_each (pos, &pgcache_hashtable.buckets[bucket]) {
			struct pgcache *candidate =
				hlist_entry(pos, struct pgcache, hash_node);

			if (candidate->data != data)
				continue;
			candidate->refcount++;
			list_move_tail(&candidate->lru_node, &pgcache_lru);
			page = candidate;
			break;
		}
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return page;
}

struct pgcache *pgcache_find_mapping(struct page_mapping *mapping,
					uint64_t index)
{
	struct list_head *pos;
	struct pgcache *page = NULL;
	irq_flags_t flags;
	if (!mapping)
		return NULL;
	spin_lock_irqsave(&pgcache_lock, &flags);
	list_for_each (pos, &pgcache_associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);
		if (assoc->mapping == mapping && assoc->index == index) {
			page = assoc->page;
			page->refcount++;
			list_move_tail(&page->lru_node, &pgcache_lru);
			break;
		}
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return page;
}

static void pgcache_free_page(struct pgcache *page,
			      struct list_head *removed)
{
	if (!page)
		return;
	pgcache_assoc_free_list(removed);
	if (page->data)
		free_page(page->data, 0);
	kfree(page);
}

static void pgcache_detach_page_locked(struct pgcache *page,
				       struct list_head *removed)
{
	if (!page || page->dropped)
		return;
	pgcache_clear_dirty_locked(page);
	pgcache_assoc_remove_page_locked(page, removed);
	if (!hlist_unhashed(&page->hash_node))
		hlist_del_init(&page->hash_node);
	if (!list_empty(&page->lru_node))
		list_del_init(&page->lru_node);
	page->dropped = true;
	if (page_cache_pages > 0)
		page_cache_pages--;
}

static struct pgcache *pgcache_evict_one_locked(struct list_head *removed)
{
	struct list_head *pos, *next;

	list_for_each_safe (pos, next, &pgcache_lru) {
		struct pgcache *page =
			list_entry(pos, struct pgcache, lru_node);
		if (page->refcount || page->dirty || page->writeback)
			continue;
		pgcache_detach_page_locked(page, removed);
		return page;
	}
	return NULL;
}

static struct pgcache *pgcache_alloc(dev_t dev, uint64_t block)
{
	struct pgcache *page;

	page = kmalloc(sizeof(*page), ALLOC_NOWAIT);
	if (!page)
		return NULL;
	memset(page, 0, sizeof(*page));
	page->data = get_free_page(0, ALLOC_NOWAIT);
	if (!page->data) {
		kfree(page);
		return NULL;
	}
	memset(page->data, 0, BLOCK_SIZE);
	page->dev = dev;
	page->block = block;
	INIT_HLIST_NODE(&page->hash_node);
	INIT_LIST_HEAD(&page->lru_node);
	INIT_LIST_HEAD(&page->dirty_node);
	return page;
}

static int pgcache_read_physical(struct pgcache *page)
{
	struct blkdev *bdev;
	int ret;
	if (!page)
		return -EINVAL;
	bdev = lookup_blkdev(page->dev);
	if (!bdev || !bdev->bd_ops || !bdev->bd_ops->read_sectors)
		return -ENXIO;
	ret = bdev->bd_ops->read_sectors(
		bdev, page->data, page->block * BLOCK_SECTORS, BLOCK_SECTORS);
	if (ret == 0)
		page->uptodate = true;
	return ret;
}

struct pgcache *pgcache_get(dev_t dev, uint64_t block, uint32_t flags,
			       int *error)
{
	struct pgcache *page;
	irq_flags_t irq_flags;
	int ret = 0;

	if (error)
		*error = 0;
	if (!lookup_blkdev(dev)) {
		if (error)
			*error = -ENXIO;
		return NULL;
	}
	if (!blkdev_block_valid(dev, block)) {
		if (error)
			*error = -EIO;
		return NULL;
	}
	pgcache_init();
retry: {
	LIST_HEAD(removed);
	struct pgcache *victim = NULL;
	bool no_room = false;

	spin_lock_irqsave(&pgcache_lock, &irq_flags);
	page = pgcache_find(dev, block);
	if (!page && !(flags & PAGE_CACHE_CREATE)) {
		spin_unlock_irqrestore(&pgcache_lock, irq_flags);
		if (error)
			*error = -ENODATA;
		return NULL;
	}
	if (page) {
		page->refcount++;
		list_move_tail(&page->lru_node, &pgcache_lru);
		spin_unlock_irqrestore(&pgcache_lock, irq_flags);
		goto read_page;
	}
	if (page_cache_pages >= PGCACHE_NR_PAGES) {
		victim = pgcache_evict_one_locked(&removed);
		if (!victim)
			no_room = true;
	}
	spin_unlock_irqrestore(&pgcache_lock, irq_flags);

	if (victim)
		pgcache_free_page(victim, &removed);
	else
		pgcache_assoc_free_list(&removed);
	if (no_room) {
		page = pgcache_dirty_any();
		if (page) {
			ret = pgcache_sync_page(page);
			pgcache_put_page(page);
			if (ret == 0)
				goto retry;
			if (error)
				*error = ret;
			return NULL;
		}
		if (error)
			*error = -ENOMEM;
		return NULL;
	}

	page = pgcache_alloc(dev, block);
	if (!page) {
		if (error)
			*error = -ENOMEM;
		return NULL;
	}
	spin_lock_irqsave(&pgcache_lock, &irq_flags);
	{
		struct pgcache *existing = pgcache_find(dev, block);

		if (existing) {
			existing->refcount++;
			list_move_tail(&existing->lru_node, &pgcache_lru);
			spin_unlock_irqrestore(&pgcache_lock, irq_flags);
			pgcache_free_page(page, NULL);
			page = existing;
			goto read_page;
		}
	}
	page->refcount = 1;
	hash_table_add(&pgcache_hashtable, pgcache_hash(dev, block),
		       &page->hash_node);
	list_add_tail(&page->lru_node, &pgcache_lru);
	page_cache_pages++;
	spin_unlock_irqrestore(&pgcache_lock, irq_flags);

read_page:
	if ((flags & PAGE_CACHE_READ) && !pgcache_is_uptodate(page)) {
		ret = pgcache_read_physical(page);
		if (ret < 0) {
			pgcache_put_page(page);
			if (error)
				*error = ret;
			return NULL;
		}
	}
	return page;
}
}

struct pgcache *pgcache_get_mapping(struct page_mapping *mapping,
				       uint64_t index, uint32_t flags,
				       int *error)
{
	uint64_t block;
	struct pgcache *page;
	struct pgcache *associated;
	int ret;
	if (error)
		*error = 0;
	if (!mapping || !mapping->ops || !mapping->ops->resolve) {
		if (error)
			*error = -EINVAL;
		return NULL;
	}
	associated = pgcache_find_mapping(mapping, index);
	if (associated) {
		page = associated;
		if ((flags & PAGE_CACHE_READ) && !pgcache_is_uptodate(page)) {
			ret = pgcache_read_physical(page);
			if (ret < 0) {
				pgcache_put_page(page);
				if (error)
					*error = ret;
				return NULL;
			}
		}
		return page;
	}
	ret = mapping->ops->resolve(mapping, index, flags & PAGE_CACHE_CREATE,
				    &block);
	if (ret < 0) {
		if (error)
			*error = ret;
		return NULL;
	}
	page = pgcache_get(
		mapping->dev, block,
		flags | ((flags & PAGE_CACHE_READ) ? PAGE_CACHE_CREATE : 0),
		&ret);
	if (page && page_cache_assoc_add(mapping, index, page) < 0) {
		pgcache_put_page(page);
		page = NULL;
		if (error)
			*error = -ENOMEM;
	}
	if (!page && error)
		*error = ret ? ret : -ENOMEM;
	return page;
}

struct pgcache *pgcache_get_block(dev_t dev, uint64_t block)
{
	return pgcache_get(dev, block, PAGE_CACHE_READ | PAGE_CACHE_CREATE,
			   NULL);
}

void pgcache_put_page(struct pgcache *page)
{
	LIST_HEAD(removed);
	irq_flags_t flags;
	bool release = false;

	if (!page)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	BUG_ON(page->refcount == 0);
	page->refcount--;
	if (page->refcount == 0 && page->dropped) {
		pgcache_detach_page_locked(page, &removed);
		release = true;
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	if (release)
		pgcache_free_page(page, &removed);
}

uint8_t *page_cache_data(struct pgcache *page)
{
	return page ? page->data : NULL;
}
bool pgcache_is_uptodate(const struct pgcache *page)
{
	irq_flags_t flags;
	bool uptodate;

	if (!page)
		return false;
	spin_lock_irqsave(&pgcache_lock, &flags);
	uptodate = page->uptodate;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return uptodate;
}

void pgcache_set_uptodate(struct pgcache *page, bool uptodate)
{
	irq_flags_t flags;

	if (!page)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	page->uptodate = uptodate;
	spin_unlock_irqrestore(&pgcache_lock, flags);
}

bool pgcache_is_dirty(const struct pgcache *page)
{
	irq_flags_t flags;
	bool dirty;

	if (!page)
		return false;
	spin_lock_irqsave(&pgcache_lock, &flags);
	dirty = page->dirty;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return dirty;
}

void pgcache_truncate_mapping(struct page_mapping *mapping, uint64_t size)
{
	struct list_head *pos, *next;
	LIST_HEAD(removed);
	uint64_t tail_index;
	uint32_t tail_offset;
	irq_flags_t flags;

	if (!mapping)
		return;
	tail_index = size / BLOCK_SIZE;
	tail_offset = (uint32_t)(size % BLOCK_SIZE);

	spin_lock_irqsave(&pgcache_lock, &flags);
	list_for_each_safe (pos, next, &pgcache_associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);
		struct pgcache *page = assoc->page;

		if (assoc->mapping != mapping)
			continue;
		if (assoc->index < tail_index)
			continue;
		if (assoc->index == tail_index && tail_offset != 0) {
			memset(page->data + tail_offset, 0,
			       BLOCK_SIZE - tail_offset);
			page->uptodate = true;
			if (!page->dirty)
				list_add_tail(&page->dirty_node,
					      &pgcache_dirty_list);
			page->dirty = true;
			continue;
		}
		list_del_init(&assoc->mapping_node);
		list_add_tail(&assoc->page_node, &removed);
		if (!pgcache_assoc_has_page_locked(page)) {
			pgcache_clear_dirty_locked(page);
			page->uptodate = false;
		}
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	pgcache_assoc_free_list(&removed);
}

void pgcache_invalidate_mapping(struct page_mapping *mapping)
{
	pgcache_assoc_remove_mapping(mapping);
}

void pgcache_truncate_inode(struct inode *inode, uint64_t size)
{
	if (inode)
		pgcache_truncate_mapping(&inode->i_pages, size);
}

void pgcache_invalidate_inode(struct inode *inode)
{
	if (inode)
		pgcache_invalidate_mapping(&inode->i_pages);
}

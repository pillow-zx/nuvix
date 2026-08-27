/* Physical page-cache sync and writeback. */

#include "internal.h"

#include <nuvix/blkdev.h>
#include <nuvix/buddy.h>
#include <nuvix/string.h>
#include <nuvix/errno.h>
#include <nuvix/spinlock.h>
#include <nuvix/worker.h>

static bool pgcache_has_mapping_locked(struct pgcache *page,
					  struct page_mapping *mapping)
{
	struct list_head *pos;

	if (!mapping)
		return true;
	list_for_each (pos, &pgcache_associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);

		if (assoc->page == page && assoc->mapping == mapping)
			return true;
	}
	return false;
}

static bool pgcache_has_mapping_index_locked(struct pgcache *page,
						struct page_mapping *mapping,
						uint64_t index)
{
	struct list_head *pos;

	list_for_each (pos, &pgcache_associations) {
		struct pgcache_assoc *assoc = list_entry(
			pos, struct pgcache_assoc, mapping_node);

		if (assoc->page == page && assoc->mapping == mapping &&
		    assoc->index == index)
			return true;
	}
	return false;
}

void pgcache_wb_init(void)
{
	/* Writeback snapshots are allocated by their producer. */
}

static int pgcache_write_snapshot(struct pgcache *page, uint8_t *snapshot)
{
	struct blkdev *bdev;
	int ret;
	if (!page)
		return -EINVAL;
	bdev = lookup_blkdev(page->dev);
	if (!bdev || !bdev->bd_ops || !bdev->bd_ops->write_sectors)
		return -ENXIO;
	ret = bdev->bd_ops->write_sectors(
		bdev, snapshot, page->block * BLOCK_SECTORS, BLOCK_SECTORS);
	return ret;
}

static int pgcache_sync_page_snapshot(struct pgcache *page,
					  bool allow_writable_lease)
{
	uint8_t *snapshot;
	irq_flags_t flags;
	uint64_t generation;
	int ret;

	if (!page)
		return -EINVAL;
	snapshot = get_free_page(0, ALLOC_NOWAIT);
	if (!snapshot)
		return -ENOMEM;
	spin_lock_irqsave(&pgcache_lock, &flags);
	if (page->writeback || page->filling || page->invalidating ||
	    (!allow_writable_lease && page->writable_pte_count != 0)) {
		spin_unlock_irqrestore(&pgcache_lock, flags);
		free_page(snapshot, 0);
		return -EBUSY;
	}
	/* An explicit page sync is also the commit point for callers that
	 * mutated metadata in place. */
	if (!page->dirty)
		list_add_tail(&page->dirty_node, &pgcache_dirty_list);
	page->dirty = true;
	page->error = 0;
	page->dirty_generation++;
	page->writeback = true;
	generation = page->dirty_generation;
	page->wb_generation = generation;
	page->refcount++; /* producer reference */
	memcpy(snapshot, page->data, BLOCK_SIZE);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	ret = pgcache_write_snapshot(page, snapshot);
	spin_lock_irqsave(&pgcache_lock, &flags);
	page->writeback = false;
	if (ret < 0)
		page->error = ret;
	else if (page->dirty_generation == generation &&
		 page->writable_pte_count == 0)
		pgcache_clear_dirty_locked(page);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	wait_channel_wake_all(&page->waitq);
	pgcache_put_page(page); /* producer reference */
	free_page(snapshot, 0);
	return ret;
}

int pgcache_sync_page(struct pgcache *page)
{
	return pgcache_sync_page_snapshot(page, false);
}

int pgcache_wb_run(struct pgcache *start, struct page_mapping *mapping)
{
	irq_flags_t flags;
	bool associated;

	if (!start)
		return -EINVAL;
	spin_lock_irqsave(&pgcache_lock, &flags);
	associated = pgcache_has_mapping_locked(start, mapping);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	if (!associated)
		return -ENOENT;
	return pgcache_sync_page(start);
}

static int pgcache_msync_run(struct pgcache *page,
				     struct page_mapping *mapping, uint64_t index)
{
	irq_flags_t flags;
	bool associated;

	spin_lock_irqsave(&pgcache_lock, &flags);
	associated = pgcache_has_mapping_index_locked(page, mapping, index);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	if (!associated)
		return -ENOENT;
	/* ponytail: CPU0-only non-preemptible tasks make this snapshot stable;
	 * add write protection or a page sequence before concurrent user writes. */
	return pgcache_sync_page_snapshot(page, true);
}

int pgcache_sync_mapping(struct page_mapping *mapping)
{
	struct pgcache *page;
	struct list_head *pos;
	irq_flags_t flags;
	if (!mapping)
		return -EINVAL;
	for (;;) {
		page = NULL;
		spin_lock_irqsave(&pgcache_lock, &flags);
		list_for_each (pos, &pgcache_associations) {
			struct pgcache_assoc *assoc = list_entry(
				pos, struct pgcache_assoc, mapping_node);
			if (assoc->mapping != mapping || !assoc->page->dirty)
				continue;
			page = assoc->page;
			page->refcount++;
			break;
		}
		spin_unlock_irqrestore(&pgcache_lock, flags);
		if (!page)
			return 0;
		int ret = pgcache_wb_run(page, mapping);

		if (ret < 0) {
			pgcache_put_page(page);
			return ret;
		}
		pgcache_put_page(page);
	}
}

int pgcache_msync_mapping_range(struct page_mapping *mapping,
					uint64_t first_page, uint64_t end_page)
{
	if (!mapping || first_page > end_page)
		return -EINVAL;

	while (first_page < end_page) {
		struct pgcache *page = NULL;
		struct list_head *pos;
		irq_flags_t flags;
		uint64_t page_index = end_page;
		int ret;

		spin_lock_irqsave(&pgcache_lock, &flags);
		list_for_each (pos, &pgcache_associations) {
			struct pgcache_assoc *assoc = list_entry(
				pos, struct pgcache_assoc, mapping_node);

			if (assoc->mapping != mapping ||
			    assoc->index < first_page ||
			    assoc->index >= end_page || !assoc->page->dirty)
				continue;
			if (!page || assoc->index < page_index) {
				page = assoc->page;
				page_index = assoc->index;
			}
		}
		if (page)
			page->refcount++;
		spin_unlock_irqrestore(&pgcache_lock, flags);
		if (!page)
			return 0;

		ret = pgcache_msync_run(page, mapping, page_index);
		pgcache_put_page(page);
		if (ret < 0 && ret != -ENOENT)
			return ret;
		first_page = page_index + 1;
	}
	return 0;
}

int pgcache_sync_inode(struct inode *inode)
{
	return inode ? pgcache_sync_mapping(&inode->i_pages) : -EINVAL;
}

int pgcache_sync_all(void)
{
	struct pgcache *page;
	while ((page = pgcache_dirty_any()) != NULL) {
		int ret = pgcache_wb_run(page, NULL);
		pgcache_put_page(page);
		if (ret < 0)
			return ret;
	}
	return 0;
}

int pgcache_sync_device(dev_t dev)
{
	for (;;) {
		struct pgcache *page = NULL;
		struct list_head *pos;
		irq_flags_t flags;

		spin_lock_irqsave(&pgcache_lock, &flags);
		list_for_each (pos, &pgcache_dirty_list) {
			struct pgcache *candidate =
				list_entry(pos, struct pgcache, dirty_node);

			if (candidate->dev != dev)
				continue;
			page = candidate;
			page->refcount++;
			break;
		}
		spin_unlock_irqrestore(&pgcache_lock, flags);
		if (!page)
			return 0;
		int ret = pgcache_sync_page(page);
		pgcache_put_page(page);
		if (ret < 0)
			return ret;
	}
}

static void pgcache_wb_once(void *arg)
{
	int ret;
	(void)arg;
	ret = pgcache_sync_all();
	(void)ret;
}

void pgcache_wb_thread(void *arg)
{
	worker_run_periodic(5, pgcache_wb_once, arg);
}

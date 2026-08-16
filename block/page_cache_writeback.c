/* Physical page-cache sync and writeback. */

#include "internal.h"

#include <nuvix/blkdev.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/spinlock.h>
#include <nuvix/worker.h>

#define PAGE_CACHE_WB_MAX 32

static uint8_t *wb_buf;
static uint32_t wb_pages;
static bool wb_ready;

DEFINE_SPINLOCK(wb_buf_lock, LOCK_RANK_WB_BUF, LOCK_IRQ_TASK_ONLY);

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

void pgcache_wb_init(void)
{
	if (wb_ready)
		return;
	wb_buf = get_free_page(5, ALLOC_NOWAIT);
	if (wb_buf)
		wb_pages = PAGE_CACHE_WB_MAX;
	else {
		wb_buf = get_free_page(0, ALLOC_NOWAIT);
		wb_pages = 1;
	}
	wb_ready = true;
}

static int pgcache_write_physical(struct pgcache *page)
{
	struct blkdev *bdev;
	int ret;
	if (!page)
		return -EINVAL;
	bdev = lookup_blkdev(page->dev);
	if (!bdev || !bdev->bd_ops || !bdev->bd_ops->write_sectors)
		return -ENXIO;
	ret = bdev->bd_ops->write_sectors(
		bdev, page->data, page->block * BLOCK_SECTORS, BLOCK_SECTORS);
	return ret;
}

int pgcache_sync_page(struct pgcache *page)
{
	irq_flags_t flags;
	int ret;
	if (!page)
		return -EINVAL;
	spin_lock_irqsave(&pgcache_lock, &flags);
	if (page->writeback) {
		spin_unlock_irqrestore(&pgcache_lock, flags);
		return -EBUSY;
	}
	page->writeback = true;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	ret = pgcache_write_physical(page);
	spin_lock_irqsave(&pgcache_lock, &flags);
	page->writeback = false;
	if (ret == 0)
		pgcache_clear_dirty_locked(page);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return ret;
}

int pgcache_wb_run(struct pgcache *start, struct page_mapping *mapping)
{
	struct pgcache *pages[PAGE_CACHE_WB_MAX] = {0};
	struct list_head *pos;
	uint32_t nr = 0;
	struct blkdev *bdev;
	irq_flags_t flags;
	int ret;

	if (!start)
		return -EINVAL;
	pgcache_wb_init();
	if (!wb_buf || !wb_pages)
		return -ENOMEM;
	spin_lock_irqsave(&pgcache_lock, &flags);
	if (start->writeback || !start->dirty) {
		spin_unlock_irqrestore(&pgcache_lock, flags);
		return -EBUSY;
	}
	pages[nr++] = start;
	list_for_each (pos, &pgcache_dirty_list) {
		struct pgcache *page =
			list_entry(pos, struct pgcache, dirty_node);
		if (nr >= PAGE_CACHE_WB_MAX || nr >= wb_pages ||
		    page == start || page->dev != start->dev ||
		    page->block != start->block + nr || page->writeback ||
		    !pgcache_has_mapping_locked(page, mapping))
			continue;
		pages[nr++] = page;
	}
	for (uint32_t i = 0; i < nr; i++)
		pages[i]->writeback = true;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	bdev = lookup_blkdev(start->dev);
	if (!bdev || !bdev->bd_ops || !bdev->bd_ops->write_sectors) {
		spin_lock_irqsave(&pgcache_lock, &flags);
		for (uint32_t i = 0; i < nr; i++)
			pages[i]->writeback = false;
		spin_unlock_irqrestore(&pgcache_lock, flags);
		return -ENXIO;
	}
	spin_lock(&wb_buf_lock);
	for (uint32_t i = 0; i < nr; i++)
		memcpy(wb_buf + i * BLOCK_SIZE, pages[i]->data, BLOCK_SIZE);
	ret = bdev->bd_ops->write_sectors(
		bdev, wb_buf, start->block * BLOCK_SECTORS, nr * BLOCK_SECTORS);
	spin_unlock(&wb_buf_lock);
	spin_lock_irqsave(&pgcache_lock, &flags);
	for (uint32_t i = 0; i < nr; i++) {
		pages[i]->writeback = false;
		if (ret == 0)
			pgcache_clear_dirty_locked(pages[i]);
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return ret;
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

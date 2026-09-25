/* Nonblocking page producers. IRQ callbacks only publish page state and wake
 * continuations; user copies and filesystem traversal stay in task context. */
#include "internal.h"

#include <nuvix/blkdev.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>

struct pgcache_async_io {
	struct blk_request request;
	struct list_head done_node;
	struct pgcache *page;
	uint8_t *snapshot;
	uint64_t generation;
	bool write;
	int result;
};

static LIST_HEAD(pgcache_completed);
static DEFINE_WAIT_CHANNEL(pgcache_progress);
static DEFINE_WAIT_CHANNEL(pgcache_irq);
static DEFINE_SPINLOCK(pgcache_completion_lock);

struct wait_channel *pgcache_progress_channel(void)
{
	return &pgcache_progress;
}

void pgcache_signal_progress(void)
{
	wait_channel_wake_all(&pgcache_progress);
}

static void pgcache_async_complete(struct blk_request *request, int result)
{
	struct pgcache_async_io *io = request->private_data;
	irq_flags_t flags;
	io->result = result;
	spin_lock_irqsave(&pgcache_completion_lock, flags);
	list_add_tail(&io->done_node, &pgcache_completed);
	spin_unlock_irqrestore(&pgcache_completion_lock, flags);
	wait_channel_wake_all(&pgcache_irq);
}

static void pgcache_finish_task(struct pgcache_async_io *io)
{
	struct pgcache *page = io->page;
	irq_flags_t flags;

	spin_lock_irqsave(&pgcache_lock, flags);
	if (io->write) {
		page->writeback = false;
		if (io->result < 0)
			page->error = io->result;
		else if (page->dirty_generation == io->generation)
			pgcache_clear_dirty_locked(page);
	} else {
		page->filling = false;
		page->uptodate = io->result == 0;
		page->error = io->result;
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	wait_channel_wake_all(&page->waitq);
	pgcache_signal_progress();
	pgcache_put_page(page);
	if (io->snapshot)
		free_page(io->snapshot, 0);
	kfree(io);
}

void pgcache_async_thread(void *arg)
{
	const struct wait_deadline deadline = wait_deadline_none();
	(void)arg;
	for (;;) {
		struct wait_scope scope = {};
		struct pgcache_async_io *io = NULL;
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret = wait_scope_begin(&scope, 0, &deadline);
		BUG_ON(ret < 0);
		ret = wait_scope_prepare(&scope, &pgcache_irq, false);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&pgcache_completion_lock, flags);
		if (!list_empty(&pgcache_completed)) {
			io = list_first_entry(&pgcache_completed,
					      struct pgcache_async_io, done_node);
			list_del_init(&io->done_node);
		}
		spin_unlock_irqrestore(&pgcache_completion_lock, flags);
		if (!io)
			ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		BUG_ON(ret < 0);
		if (io)
			pgcache_finish_task(io);
	}
}

static int pgcache_start_fill(struct pgcache *page)
{
	struct pgcache_async_io *io;
	struct blkdev *bdev;
	irq_flags_t flags;
	int ret;

	io = kmalloc(sizeof(*io), ALLOC_NOWAIT);
	if (!io)
		return -ENOMEM;
	bdev = lookup_blkdev(page->dev);
	if (!bdev || !bdev->bd_ops || !bdev->bd_ops->submit) {
		kfree(io);
		return -ENXIO;
	}
	spin_lock_irqsave(&pgcache_lock, flags);
	if (page->uptodate || page->filling || page->writeback || page->error) {
		ret = page->uptodate ? 0 : page->error ? page->error : -EAGAIN;
		spin_unlock_irqrestore(&pgcache_lock, flags);
		kfree(io);
		return ret;
	}
	page->filling = true;
	page->refcount++;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	*io = (struct pgcache_async_io){
		.page = page,
		.request = {
			.op = BLK_REQUEST_READ, .buffer = page->data,
			.sector = page->block * BLOCK_SECTORS,
			.nsec = BLOCK_SECTORS,
			.complete = pgcache_async_complete,
			.private_data = io,
		},
	};
	ret = bdev->bd_ops->submit(bdev, &io->request);
	if (ret < 0) {
		spin_lock_irqsave(&pgcache_lock, flags);
		page->filling = false;
		page->error = ret;
		spin_unlock_irqrestore(&pgcache_lock, flags);
		pgcache_put_page(page);
		kfree(io);
		return ret;
	}
	return -EAGAIN;
}

static int pgcache_start_writeback(struct pgcache *page)
{
	struct pgcache_async_io *io;
	struct blkdev *bdev;
	irq_flags_t flags;
	int ret;

	io = kmalloc(sizeof(*io), ALLOC_NOWAIT);
	if (!io)
		return -ENOMEM;
	io->snapshot = get_page(0, ALLOC_NOWAIT);
	if (!io->snapshot) {
		kfree(io);
		return -ENOMEM;
	}
	bdev = lookup_blkdev(page->dev);
	if (!bdev || !bdev->bd_ops || !bdev->bd_ops->submit) {
		free_page(io->snapshot, 0);
		kfree(io);
		return -ENXIO;
	}
	spin_lock_irqsave(&pgcache_lock, flags);
	if (!page->dirty || page->writeback || page->filling || page->error) {
		ret = !page->dirty ? 0 : page->error ? page->error : -EAGAIN;
		spin_unlock_irqrestore(&pgcache_lock, flags);
		free_page(io->snapshot, 0);
		kfree(io);
		return ret;
	}
	page->writeback = true;
	page->refcount++;
	io->generation = page->dirty_generation;
	memcpy(io->snapshot, page->data, BLOCK_SIZE);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	io->page = page;
	io->write = true;
	io->request = (struct blk_request){
		.op = BLK_REQUEST_WRITE, .buffer = io->snapshot,
		.sector = page->block * BLOCK_SECTORS,
		.nsec = BLOCK_SECTORS,
		.complete = pgcache_async_complete,
		.private_data = io,
	};
	ret = bdev->bd_ops->submit(bdev, &io->request);
	if (ret < 0) {
		spin_lock_irqsave(&pgcache_lock, flags);
		page->writeback = false;
		page->error = ret;
		spin_unlock_irqrestore(&pgcache_lock, flags);
		pgcache_put_page(page);
		free_page(io->snapshot, 0);
		kfree(io);
		return ret;
	}
	return -EAGAIN;
}

static int pgcache_make_room(void)
{
	struct pgcache *dirty = pgcache_reclaimable_dirty();
	int ret;
	if (dirty) {
		ret = pgcache_start_writeback(dirty);
		pgcache_put_page(dirty);
		if (ret < 0 && ret != -EAGAIN)
			return ret;
		if (ret == -EAGAIN)
			return -EAGAIN;
	}
	return pgcache_room_pending() ? -EAGAIN : -ENOMEM;
}

int pgcache_try_read_block(dev_t dev, uint64_t block, struct pgcache **out)
{
	struct pgcache *page;
	int ret = 0;
	*out = NULL;
	page = pgcache_get(dev, block, PAGE_CACHE_CREATE | PAGE_CACHE_NOWAIT,
			   &ret);
	if (!page) {
		if (ret == -EAGAIN)
			ret = pgcache_make_room();
		return ret;
	}
	ret = pgcache_start_fill(page);
	if (ret == 0)
		*out = page;
	else
		pgcache_put_page(page);
	return ret;
}

int pgcache_try_create_block(dev_t dev, uint64_t block, struct pgcache **out)
{
	int ret = 0;
	*out = pgcache_get(dev, block, PAGE_CACHE_CREATE | PAGE_CACHE_NOWAIT,
			   &ret);
	if (!*out && ret == -EAGAIN)
		ret = pgcache_make_room();
	return *out ? 0 : ret;
}

int pgcache_try_get_mapping(struct page_mapping *mapping, uint64_t index,
			    uint32_t flags, struct pgcache **out)
{
	struct pgcache *page;
	uint64_t block;
	int ret = 0;
	*out = NULL;
	if (!mapping || !mapping->ops || !mapping->ops->resolve_nowait)
		return -EOPNOTSUPP;
	page = pgcache_find_mapping(mapping, index);
	if (!page) {
		ret = mapping->ops->resolve_nowait(mapping, index,
						  !!(flags & PAGE_CACHE_CREATE),
						  &block);
		if (ret < 0)
			return ret;
		page = pgcache_get(mapping->dev, block,
				   PAGE_CACHE_CREATE | PAGE_CACHE_NOWAIT, &ret);
		if (!page) {
			if (ret == -EAGAIN)
				ret = pgcache_make_room();
			return ret;
		}
		ret = page_cache_assoc_add(mapping, index, page);
		if (ret < 0) {
			pgcache_put_page(page);
			return ret;
		}
	}
	if (flags & PAGE_CACHE_READ)
		ret = pgcache_start_fill(page);
	if (ret == 0)
		*out = page;
	else
		pgcache_put_page(page);
	return ret;
}

static int pgcache_sync_step(struct page_mapping *mapping, dev_t dev,
			     bool device)
{
	struct pgcache *page = NULL;
	struct list_head *pos;
	irq_flags_t flags;
	int ret;
	spin_lock_irqsave(&pgcache_lock, flags);
	if (device) {
		list_for_each(pos, &pgcache_dirty_list) {
			struct pgcache *candidate =
				list_entry(pos, struct pgcache, dirty_node);
			if (candidate->dev == dev) {
				page = candidate;
				break;
			}
		}
	} else {
		list_for_each(pos, &mapping->pages) {
			struct pgcache_assoc *assoc =
				list_entry(pos, struct pgcache_assoc, mapping_node);
			if (assoc->page->dirty) {
				page = assoc->page;
				break;
			}
		}
	}
	if (page)
		page->refcount++;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	if (!page)
		return 0;
	ret = pgcache_start_writeback(page);
	pgcache_put_page(page);
	return ret;
}

int pgcache_sync_mapping_step(struct page_mapping *mapping)
{
	return mapping ? pgcache_sync_step(mapping, 0, false) : -EINVAL;
}

int pgcache_sync_device_step(dev_t dev)
{
	return pgcache_sync_step(NULL, dev, true);
}

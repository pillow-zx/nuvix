#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/string.h>
#include <nuvix/page_cache.h>

#include "ext2.h"

static bool ext2_file_index_valid(uint64_t index)
{
	return index <= EXT2_MAX_FILE_INDEX;
}

static int ext2_resolve_block(struct page_mapping *mapping, uint64_t index,
			      bool create, uint64_t *block)
{
	struct inode *inode = mapping ? mapping->host : NULL;
	uint32_t pblock;
	int ret;

	if (!inode || !block)
		return -EINVAL;
	if (!ext2_file_index_valid(index))
		return -EFBIG;
	if (create) {
		ret = ext2_bmap(inode, (uint32_t)index, true, &pblock);
		if (ret < 0)
			return ret;
	} else {
		ret = ext2_bmap_readonly(inode, (uint32_t)index, &pblock);
		if (ret < 0)
			return ret;
	}
	if (!pblock)
		return create ? -ENOSPC : -ENODATA;
	*block = pblock;
	return 0;
}

static int ext2_resolve_block_nowait(struct page_mapping *mapping,
				      uint64_t index, bool create,
				      uint64_t *block)
{
	struct inode *inode = mapping ? mapping->host : NULL;
	uint32_t pblock;
	int ret;
	if (!inode || !block)
		return -EINVAL;
	if (!ext2_file_index_valid(index))
		return -EFBIG;
	ret = ext2_bmap_nowait(inode, (uint32_t)index, create, &pblock);
	if (ret < 0)
		return ret;
	if (!pblock)
		return create ? -ENOSPC : -ENODATA;
	*block = pblock;
	return 0;
}

const struct page_mapping_ops ext2_inode_mapping_ops = {
	.resolve = ext2_resolve_block,
	.resolve_nowait = ext2_resolve_block_nowait,
};

ssize_t ext2_read_file(struct inode *inode, char *buf, size_t count, loff_t pos)
{
	size_t done = 0;
	uint64_t readable_size;

	if (!inode || !buf)
		return -EINVAL;
	if (pos < 0)
		return -EINVAL;

	readable_size = inode->i_size;
	if (readable_size > EXT2_MAX_FILE_SIZE)
		readable_size = EXT2_MAX_FILE_SIZE;

	if ((uint64_t)pos >= readable_size)
		return 0;
	if (count > readable_size - (uint64_t)pos)
		count = readable_size - (uint64_t)pos;

	while (done < count) {
		struct pgcache *page;
		uint64_t file_pos = (uint64_t)pos + done;
		uint32_t lblock = (uint32_t)(file_pos / BLOCK_SIZE);
		uint32_t offset = (uint32_t)(file_pos % BLOCK_SIZE);
		size_t chunk = BLOCK_SIZE - offset;

		if (chunk > count - done)
			chunk = count - done;

		int error;
		page = pgcache_get_mapping(&inode->i_pages, lblock,
					   PAGE_CACHE_READ, &error);
		if (!page && error == -ENODATA) {
			memset(buf + done, 0, chunk);
			done += chunk;
			continue;
		}
		if (!page)
			return done ? (ssize_t)done : -EIO;

		memcpy(buf + done, page_cache_data(page) + offset, chunk);
		pgcache_put_page(page);
		done += chunk;
	}

	return (ssize_t)done;
}

ssize_t ext2_write_file(struct inode *inode, const char *buf, size_t count,
			loff_t pos)
{
	size_t done = 0;
	uint64_t writable;
	int ret;

	if (!inode || !buf)
		return -EINVAL;
	if (pos < 0)
		return -EINVAL;
	if ((uint64_t)pos >= EXT2_MAX_FILE_SIZE)
		return count == 0 ? 0 : -EFBIG;

	writable = EXT2_MAX_FILE_SIZE - (uint64_t)pos;
	if ((uint64_t)count > writable)
		count = (size_t)writable;

	while (done < count) {
		struct pgcache *page;
		uint64_t file_pos = (uint64_t)pos + done;
		uint32_t lblock = (uint32_t)(file_pos / BLOCK_SIZE);
		uint32_t offset = (uint32_t)(file_pos % BLOCK_SIZE);
		size_t chunk = BLOCK_SIZE - offset;
		int error;
		if (chunk > count - done)
			chunk = count - done;

		page = pgcache_get_mapping(&inode->i_pages, lblock,
					   (offset == 0 && chunk == BLOCK_SIZE)
						   ? PAGE_CACHE_CREATE
						   : PAGE_CACHE_READ |
							     PAGE_CACHE_CREATE,
					   &error);
		if (!page) {
			if (!done)
				return error ? error : -ENOMEM;
			break;
		}

		memcpy(page_cache_data(page) + offset, buf + done, chunk);
		pgcache_mark_dirty(page);
		pgcache_put_page(page);
		done += chunk;
	}

	ret = 0;
	if (done && (uint64_t)pos + done > inode->i_size) {
		mutex_lock(&inode->i_lock);
		if ((uint64_t)pos + done > inode->i_size) {
			inode->i_size = (uint64_t)pos + done;
			ret = ext2_mark_inode_dirty(inode);
		}
		mutex_unlock(&inode->i_lock);
		if (ret < 0)
			return ret;
	}

	return (ssize_t)done;
}

static ssize_t ext2_file_read(struct file *file, char *buf, size_t count,
			      loff_t pos)
{
	return ext2_read_file(file->f_inode, buf, count, pos);
}

static ssize_t ext2_file_write(struct file *file, const char *buf, size_t count,
			       loff_t pos)
{
	return ext2_write_file(file->f_inode, buf, count, pos);
}

static ssize_t ext2_file_try_io_pos(struct file *file, void *buf, size_t count,
				     loff_t pos, bool write)
{
	struct inode *inode = file ? file->f_inode : NULL;
	struct pgcache *page = NULL;
	struct pgcache *inode_table = NULL;
	uint64_t position;
	uint32_t offset;
	size_t chunk;
	int ret;

	if (!inode || !buf || pos < 0)
		return -EINVAL;
	if (!count)
		return 0;
	position = (uint64_t)pos;
	if (write) {
		if (position >= EXT2_MAX_FILE_SIZE)
			return -EFBIG;
		if (count > EXT2_MAX_FILE_SIZE - position)
			count = EXT2_MAX_FILE_SIZE - position;
		ret = ext2_pin_inode_table_nowait(inode, &inode_table);
		if (ret < 0)
			return ret;
	} else {
		uint64_t end = MIN(inode->i_size, (uint64_t)EXT2_MAX_FILE_SIZE);
		if (position >= end)
			return 0;
		if (count > end - position)
			count = end - position;
	}
	offset = (uint32_t)(position % BLOCK_SIZE);
	chunk = MIN(count, (size_t)(BLOCK_SIZE - offset));
	ret = pgcache_try_get_mapping(&inode->i_pages, position / BLOCK_SIZE,
				      write ? PAGE_CACHE_CREATE |
					      ((offset || chunk != BLOCK_SIZE) ?
					       PAGE_CACHE_READ : 0) :
					      PAGE_CACHE_READ, &page);
	if (ret == -ENODATA && !write) {
		memset(buf, 0, chunk);
		ret = (int)chunk;
		goto out;
	}
	if (ret < 0)
		goto out;
	if (write) {
		if (!mutex_trylock(&inode->i_lock)) {
			ret = -EAGAIN;
			goto out;
		}
		memcpy(page_cache_data(page) + offset, buf, chunk);
		pgcache_mark_dirty(page);
		if (position + chunk > inode->i_size) {
			inode->i_size = position + chunk;
			ret = ext2_mark_inode_dirty(inode);
		} else {
			ret = 0;
		}
		mutex_unlock(&inode->i_lock);
	} else {
		memcpy(buf, page_cache_data(page) + offset, chunk);
	}
	if (ret == 0)
		ret = (int)chunk;
out:
	pgcache_put_page(page);
	pgcache_put_page(inode_table);
	return ret;
}

static int ext2_file_try_fsync_prepare(struct file *file, bool datasync)
{
	struct pgcache *inode_table = NULL;
	int ret;
	(void)datasync;
	ret = ext2_pin_inode_table_nowait(file->f_inode, &inode_table);
	if (ret < 0)
		return ret;
	if (!mutex_trylock(&file->f_inode->i_lock)) {
		pgcache_put_page(inode_table);
		return -EAGAIN;
	}
	ret = ext2_mark_inode_dirty(file->f_inode);
	mutex_unlock(&file->f_inode->i_lock);
	pgcache_put_page(inode_table);
	return ret;
}

const struct file_operations ext2_file_operations = {
	.read = ext2_file_read,
	.write = ext2_file_write,
	.try_io_pos = ext2_file_try_io_pos,
	.try_fsync_prepare = ext2_file_try_fsync_prepare,
};

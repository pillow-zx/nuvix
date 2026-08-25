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

const struct page_mapping_ops ext2_inode_mapping_ops = {
	.resolve = ext2_resolve_block,
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
			return done ? (ssize_t)done : (error ? error : -ENOMEM);
		}

		memcpy(page_cache_data(page) + offset, buf + done, chunk);
		pgcache_mark_dirty(page);
		pgcache_put_page(page);
		done += chunk;
	}

	ret = 0;
	if ((uint64_t)pos + done > inode->i_size) {
		mutex_lock(&inode->i_lock);
		if ((uint64_t)pos + done > inode->i_size) {
			inode->i_size = (uint64_t)pos + done;
			ret = ext2_write_inode(inode);
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

const struct file_operations ext2_file_operations = {
	.read = ext2_file_read,
	.write = ext2_file_write,
};

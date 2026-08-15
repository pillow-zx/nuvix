#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/slab.h>
#include <nuvix/stat.h>

#include "ext2.h"

static uint32_t ext2_encode_dev(dev_t dev)
{
	return (MAJOR(dev) << 8) | (dev & 0xff);
}

static int ext2_sync_metadata_page(struct pgcache *page)
{
	return pgcache_sync_page(page);
}

static dev_t ext2_decode_dev(uint32_t raw)
{
	uint32_t major = (raw >> 8) & 0xff;
	uint32_t minor = raw & 0xff;

	return MKDEV(major, minor);
}

static inline uint32_t ext2_branch_span(int depth)
{
	uint32_t ptrs = BLOCK_SIZE / sizeof(uint32_t);
	uint32_t span = 1;

	for (int i = 0; i < depth; i++)
		span *= ptrs;
	return span;
}

static const struct inode_operations ext2_file_inode_operations;
static uint32_t ext2_bmap_ro_scratch[BLOCK_SIZE / sizeof(uint32_t)];

static uint32_t *ext2_block_words(struct pgcache *page)
{
	static_assert(BLOCK_SIZE % sizeof(uint32_t) == 0,
		      "ext2 indirect blocks are uint32_t arrays");

	return (uint32_t *)(uintptr_t)page_cache_data(page);
}

static int ext2_inode_location(struct inode *inode, uint32_t *block,
			       uint32_t *offset)
{
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);
	uint32_t ino = (uint32_t)inode->i_ino;
	uint32_t group;
	uint32_t index;
	uint32_t byte_offset;

	if (ino == 0)
		return -EINVAL;

	group = (ino - 1) / sbi->s_inodes_per_group;
	index = (ino - 1) % sbi->s_inodes_per_group;
	if (group >= sbi->s_groups_count)
		return -EINVAL;

	byte_offset = index * sbi->s_inode_size;
	*block = sbi->s_group_desc[group].bg_inode_table +
		 byte_offset / BLOCK_SIZE;
	*offset = byte_offset % BLOCK_SIZE;

	return 0;
}

static void ext2_free_indirect_chain(struct super_block *sb, uint32_t block,
				     int depth)
{
	uint32_t ptrs = BLOCK_SIZE / sizeof(uint32_t);
	struct pgcache *page;
	uint32_t *entries;

	if (!block)
		return;
	if (depth == 0) {
		ext2_free_block(sb, block);
		return;
	}

	page = pgcache_get_block(sb->s_dev, block);
	if (page) {
		entries = ext2_block_words(page);
		for (uint32_t i = 0; i < ptrs; i++) {
			if (!entries[i])
				continue;
			ext2_free_indirect_chain(sb, entries[i], depth - 1);
		}
		pgcache_put_page(page);
	}

	ext2_free_block(sb, block);
}

void ext2_free_inode_blocks(struct inode *inode)
{
	struct ext2_inode *raw = &EXT2_I(inode)->raw_inode;

	for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
		if (raw->i_block[i])
			ext2_free_block(inode->i_sb, raw->i_block[i]);
		raw->i_block[i] = 0;
	}

	ext2_free_indirect_chain(inode->i_sb, raw->i_block[EXT2_IND_BLOCK], 1);
	raw->i_block[EXT2_IND_BLOCK] = 0;
	ext2_free_indirect_chain(inode->i_sb, raw->i_block[EXT2_DIND_BLOCK], 2);
	raw->i_block[EXT2_DIND_BLOCK] = 0;
	ext2_free_indirect_chain(inode->i_sb, raw->i_block[EXT2_TIND_BLOCK], 3);
	raw->i_block[EXT2_TIND_BLOCK] = 0;

	raw->i_blocks = 0;
	raw->i_size = 0;
	inode->i_size = 0;
	inode->i_blocks = 0;
}

static uint32_t ext2_count_tree_blocks(struct super_block *sb, uint32_t block,
				       int depth)
{
	uint32_t ptrs = BLOCK_SIZE / sizeof(uint32_t);
	struct pgcache *page;
	uint32_t *entries;
	uint32_t total = 1;

	if (!block)
		return 0;
	if (depth == 0)
		return 1;

	page = pgcache_get_block(sb->s_dev, block);
	if (!page)
		return total;

	entries = ext2_block_words(page);
	for (uint32_t i = 0; i < ptrs; i++) {
		if (!entries[i])
			continue;
		total += ext2_count_tree_blocks(sb, entries[i], depth - 1);
	}
	pgcache_put_page(page);
	return total;
}

static int ext2_truncate_branch_slot(struct inode *inode, uint32_t *slot,
				     int depth, uint32_t keep_blocks)
{
	uint32_t ptrs = BLOCK_SIZE / sizeof(uint32_t);
	struct pgcache *page;
	uint32_t *entries;
	uint32_t span;
	uint32_t remaining = keep_blocks;
	bool all_zero = true;

	if (!slot || !*slot)
		return 0;
	if (keep_blocks == 0) {
		ext2_free_indirect_chain(inode->i_sb, *slot, depth);
		*slot = 0;
		return 0;
	}
	if (depth == 0)
		return 0;

	span = ext2_branch_span(depth - 1);
	page = pgcache_get_block(inode->i_sb->s_dev, *slot);
	if (!page)
		return -EIO;

	entries = ext2_block_words(page);
	for (uint32_t i = 0; i < ptrs; i++) {
		uint32_t child_keep = remaining > span ? span : remaining;
		int ret = ext2_truncate_branch_slot(inode, &entries[i],
						    depth - 1, child_keep);

		if (ret < 0) {
			pgcache_put_page(page);
			return ret;
		}
		if (entries[i])
			all_zero = false;
		if (remaining > child_keep)
			remaining -= child_keep;
		else
			remaining = 0;
	}

	if (all_zero) {
		pgcache_put_page(page);
		ext2_free_block(inode->i_sb, *slot);
		*slot = 0;
		return 0;
	}

	if (ext2_sync_metadata_page(page) < 0) {
		pgcache_put_page(page);
		return -EIO;
	}
	pgcache_put_page(page);
	return 0;
}

static int ext2_truncate_branch(struct inode *inode, uint32_t index, int depth,
				uint32_t keep_blocks)
{
	struct ext2_inode *raw = &EXT2_I(inode)->raw_inode;
	uint32_t slot = raw->i_block[index];
	int ret = ext2_truncate_branch_slot(inode, &slot, depth, keep_blocks);

	if (ret < 0)
		return ret;
	raw->i_block[index] = slot;
	return 0;
}

static int ext2_zero_truncate_tail(struct inode *inode, uint64_t size)
{
	uint32_t offset = (uint32_t)(size % BLOCK_SIZE);
	uint32_t lblock;
	uint32_t pblock;
	struct pgcache *page;

	if (size == 0 || offset == 0)
		return 0;

	lblock = (uint32_t)(size / BLOCK_SIZE);
	pblock = ext2_bmap_readonly(inode, lblock);
	if (!pblock)
		return 0;

	page = pgcache_get_mapping(&inode->i_pages, lblock, PAGE_CACHE_READ,
				      NULL);
	if (!page)
		return -EIO;

	memset(page_cache_data(page) + offset, 0, BLOCK_SIZE - offset);
	pgcache_mark_dirty(page);
	if (pgcache_sync_page(page) < 0) {
		pgcache_put_page(page);
		return -EIO;
	}
	pgcache_put_page(page);
	return 0;
}

static int ext2_zero_extend_tail(struct inode *inode, uint64_t old_size)
{
	struct pgcache *page;
	uint32_t offset = (uint32_t)(old_size % BLOCK_SIZE);
	uint32_t lblock;
	uint32_t pblock;

	if (!inode || old_size == 0 || offset == 0)
		return 0;

	lblock = (uint32_t)(old_size / BLOCK_SIZE);
	pblock = ext2_bmap_readonly(inode, lblock);
	if (!pblock)
		return 0;

	page = pgcache_get_mapping(&inode->i_pages, lblock, PAGE_CACHE_READ,
				      NULL);
	if (!page)
		return -EIO;

	memset(page_cache_data(page) + offset, 0, BLOCK_SIZE - offset);
	pgcache_mark_dirty(page);
	pgcache_put_page(page);
	return 0;
}

void ext2_init_inode_ops(struct inode *inode)
{
	if (!inode)
		return;

	inode->i_op = NULL;
	inode->i_fop = NULL;
	inode->i_pages.dev = inode->i_sb ? inode->i_sb->s_dev : 0;
	inode->i_pages.ops = NULL;

	switch (inode->i_mode & EXT2_S_IFMT) {
	case EXT2_S_IFDIR:
		inode->i_op = &ext2_dir_inode_operations;
		inode->i_fop = &ext2_dir_operations;
		inode->i_pages.ops = &ext2_inode_mapping_ops;
		break;
	case EXT2_S_IFLNK:
		inode->i_op = &ext2_symlink_inode_operations;
		inode->i_pages.ops = &ext2_inode_mapping_ops;
		break;
	case EXT2_S_IFCHR:
	case EXT2_S_IFBLK:
		break;
	case EXT2_S_IFREG:
	default:
		inode->i_op = &ext2_file_inode_operations;
		inode->i_fop = &ext2_file_operations;
		inode->i_pages.ops = &ext2_inode_mapping_ops;
		break;
	}
}

static void ext2_fill_vfs_inode(struct inode *inode)
{
	struct ext2_inode *raw = &EXT2_I(inode)->raw_inode;
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);

	inode->i_mode = raw->i_mode;
	inode->i_uid = raw->i_uid;
	inode->i_gid = raw->i_gid;
	if (sbi->s_es.s_creator_os == EXT2_OS_LINUX) {
		inode->i_uid |= (uint32_t)raw->i_osd2[4] << 16 |
				(uint32_t)raw->i_osd2[5] << 24;
		inode->i_gid |= (uint32_t)raw->i_osd2[6] << 16 |
				(uint32_t)raw->i_osd2[7] << 24;
	}
	inode->i_nlink = raw->i_links_count;
	inode->i_size = raw->i_size;
	inode->i_blocks = raw->i_blocks;
	inode->i_atime_sec = raw->i_atime;
	inode->i_mtime_sec = raw->i_mtime;
	inode->i_ctime_sec = raw->i_ctime;
	if ((raw->i_mode & EXT2_S_IFMT) == EXT2_S_IFCHR ||
	    (raw->i_mode & EXT2_S_IFMT) == EXT2_S_IFBLK)
		inode->i_rdev = ext2_decode_dev(raw->i_block[0]);
	else
		inode->i_rdev = 0;

	ext2_init_inode_ops(inode);
}

/* Symlink targets are immutable after creation; the raw inode mirrors are
 * stable and the target page is fetched through the page cache. */
static int ext2_readlink(struct inode *inode, char *buf, size_t size)
{
	struct ext2_inode *raw = &EXT2_I(inode)->raw_inode;
	uint64_t len = inode->i_size;

	if (!buf || size == 0)
		return -EINVAL;
	if (len > size)
		len = size;

	if (raw->i_blocks == 0) {
		if (inode->i_size > sizeof(raw->i_block))
			return -EIO;
		memcpy(buf, raw->i_block, (size_t)len);
	} else {
		struct pgcache *page;

		if (!ext2_bmap_readonly(inode, 0))
			return -EIO;
		page = pgcache_get_mapping(&inode->i_pages, 0,
					      PAGE_CACHE_READ, NULL);
		if (!page)
			return -EIO;
		if (len > BLOCK_SIZE)
			len = BLOCK_SIZE;
		memcpy(buf, page_cache_data(page), (size_t)len);
		pgcache_put_page(page);
	}

	return (int)len;
}

static int ext2_fallocate_inode(struct inode *inode, int mode, uint64_t offset,
				uint64_t len)
{
	struct ext2_inode *raw;
	uint64_t end;
	uint32_t first_block;
	uint32_t last_block;
	int ret;

	if (!inode || !inode->i_private)
		return -EINVAL;
	if (mode != 0)
		return -EINVAL;
	if (len == 0 || offset > EXT2_MAX_FILE_SIZE ||
	    len > EXT2_MAX_FILE_SIZE - offset)
		return -EFBIG;

	end = offset + len;
	first_block = (uint32_t)(offset / BLOCK_SIZE);
	last_block = (uint32_t)((end - 1) / BLOCK_SIZE);
	for (uint32_t block = first_block; block <= last_block; block++) {
		uint32_t mapped;

		ret = ext2_bmap(inode, block, true, &mapped);
		if (ret < 0)
			return ret;
		if (!mapped)
			return -ENOSPC;
	}

	raw = &EXT2_I(inode)->raw_inode;
	if (end > inode->i_size) {
		ret = ext2_zero_extend_tail(inode, inode->i_size);
		if (ret < 0)
			return ret;
		inode->i_size = end;
		raw->i_size = (uint32_t)end;
	}
	raw->i_blocks = (uint32_t)inode->i_blocks;

	return ext2_write_inode(inode);
}

const struct inode_operations ext2_symlink_inode_operations = {
	.readlink = ext2_readlink,
	.truncate = ext2_truncate_inode,
};

static const struct inode_operations ext2_file_inode_operations = {
	.truncate = ext2_truncate_inode,
	.fallocate = ext2_fallocate_inode,
};

int ext2_read_inode(struct inode *inode)
{
	struct ext2_inode_info *ei;
	struct pgcache *page;
	struct ext2_sb_info *sbi;
	uint32_t block;
	uint32_t offset;
	int ret;

	if (!inode || !inode->i_sb)
		return -EINVAL;

	ei = kmalloc(sizeof(*ei), ALLOC_NOWAIT);
	if (!ei)
		return -ENOMEM;
	memset(ei, 0, sizeof(*ei));

	ret = ext2_inode_location(inode, &block, &offset);
	if (ret < 0) {
		kfree(ei);
		return ret;
	}

	/* Page fetch runs outside s_lock (fetching may allocate); the raw
	 * inode copy happens under it for a consistent snapshot. */
	page = pgcache_get_block(inode->i_sb->s_dev, block);
	if (!page) {
		kfree(ei);
		return -EIO;
	}

	sbi = EXT2_SB(inode->i_sb);
	spin_lock(&sbi->s_lock);
	memcpy(&ei->raw_inode, page_cache_data(page) + offset,
	       sizeof(ei->raw_inode));
	inode->i_private = ei;
	ext2_fill_vfs_inode(inode);
	spin_unlock(&sbi->s_lock);

	pgcache_put_page(page);

	return 0;
}

int ext2_write_inode(struct inode *inode)
{
	struct ext2_inode_info *ei;
	struct pgcache *page;
	struct ext2_sb_info *sbi;
	uint32_t block;
	uint32_t offset;
	int ret;

	if (!inode || !inode->i_sb || !inode->i_private)
		return -EINVAL;

	ei = EXT2_I(inode);
	sbi = EXT2_SB(inode->i_sb);

	ret = ext2_inode_location(inode, &block, &offset);
	if (ret < 0)
		return ret;

	/* Page fetch and the device sync both run outside s_lock (fetch may
	 * allocate); the in-memory copy of the raw inode is the shared
	 * mutation and stays under the lock. */
	page = pgcache_get_block(inode->i_sb->s_dev, block);
	if (!page)
		return -EIO;

	spin_lock(&sbi->s_lock);
	if (sbi->s_es.s_creator_os != EXT2_OS_LINUX &&
	    (inode->i_uid > UINT16_MAX || inode->i_gid > UINT16_MAX)) {
		spin_unlock(&sbi->s_lock);
		pgcache_put_page(page);
		return -EINVAL;
	}
	ei->raw_inode.i_mode = (uint16_t)inode->i_mode;
	ei->raw_inode.i_uid = (uint16_t)inode->i_uid;
	ei->raw_inode.i_gid = (uint16_t)inode->i_gid;
	if (sbi->s_es.s_creator_os == EXT2_OS_LINUX) {
		ei->raw_inode.i_osd2[4] = (uint8_t)(inode->i_uid >> 16);
		ei->raw_inode.i_osd2[5] = (uint8_t)(inode->i_uid >> 24);
		ei->raw_inode.i_osd2[6] = (uint8_t)(inode->i_gid >> 16);
		ei->raw_inode.i_osd2[7] = (uint8_t)(inode->i_gid >> 24);
	}
	ei->raw_inode.i_links_count = (uint16_t)inode->i_nlink;
	ei->raw_inode.i_size = (uint32_t)inode->i_size;
	ei->raw_inode.i_blocks = (uint32_t)inode->i_blocks;
	ei->raw_inode.i_atime = (uint32_t)inode->i_atime_sec;
	ei->raw_inode.i_mtime = (uint32_t)inode->i_mtime_sec;
	ei->raw_inode.i_ctime = (uint32_t)inode->i_ctime_sec;
	if ((inode->i_mode & S_IFMT) == S_IFCHR ||
	    (inode->i_mode & S_IFMT) == S_IFBLK)
		ei->raw_inode.i_block[0] = ext2_encode_dev(inode->i_rdev);

	memcpy(page_cache_data(page) + offset, &ei->raw_inode,
	       sizeof(ei->raw_inode));
	spin_unlock(&sbi->s_lock);

	ret = ext2_sync_metadata_page(page);
	pgcache_put_page(page);

	return ret;
}

int ext2_datasync_inode(struct inode *inode)
{
	if (!inode || !inode->i_sb || !inode->i_private)
		return -EINVAL;

	/* Allocation metadata and file size changes are written at mutation
	 * time. */
	return 0;
}

static uint32_t ext2_alloc_bmap_block(struct inode *inode)
{
	struct ext2_inode *raw = &EXT2_I(inode)->raw_inode;
	uint32_t block = ext2_alloc_block(inode);

	if (block) {
		raw->i_blocks += BLOCK_SIZE / SECTOR_SIZE;
		inode->i_blocks = raw->i_blocks;
	}
	return block;
}

static int ext2_ind_bmap(struct inode *inode, uint32_t ind_block,
			 uint32_t index, bool create, uint32_t *mapped)
{
	struct pgcache *page;
	uint32_t *blocks;
	uint32_t block;
	int ret;

	if (!mapped)
		return -EINVAL;
	*mapped = 0;
	if (!ind_block)
		return 0;

	page = pgcache_get_block(inode->i_sb->s_dev, ind_block);
	if (!page)
		return -EIO;

	blocks = ext2_block_words(page);
	block = blocks[index];
	if (!block && create) {
		block = ext2_alloc_bmap_block(inode);
		if (block) {
			blocks[index] = block;
			ret = ext2_sync_metadata_page(page);
			if (ret < 0) {
				pgcache_put_page(page);
				return ret;
			}
			ret = ext2_write_inode(inode);
			if (ret < 0) {
				pgcache_put_page(page);
				return ret;
			}
		}
	}

	pgcache_put_page(page);
	*mapped = block;
	return 0;
}

static int ext2_read_block_words(struct super_block *sb, uint32_t block,
				 uint32_t *words)
{
	struct pgcache *page;

	if (!sb || !words || !block)
		return -EINVAL;
	page = pgcache_get_block(sb->s_dev, block);
	if (!page)
		return -EIO;
	memcpy(words, page_cache_data(page), BLOCK_SIZE);
	pgcache_put_page(page);
	return 0;
}

static uint32_t ext2_ind_bmap_readonly(struct super_block *sb,
				       uint32_t ind_block, uint32_t index)
{
	struct page_mapping *mapping;
	struct pgcache *page;
	uint32_t block;

	if (!ind_block)
		return 0;

	mapping = blkdev_pages(sb->s_dev);
	if (mapping) {
		page = pgcache_get_mapping(mapping, ind_block,
					      PAGE_CACHE_READ, NULL);
		if (page) {
			if (pgcache_is_uptodate(page))
				block = ext2_block_words(page)[index];
			else
				block = 0;
			pgcache_put_page(page);
			return block;
		}
	}

	if (ext2_read_block_words(sb, ind_block, ext2_bmap_ro_scratch) < 0)
		return 0;

	return ext2_bmap_ro_scratch[index];
}

static uint32_t ext2_inode_tree_blocks(const struct inode *inode)
{
	const struct ext2_inode_info *ei =
		(const struct ext2_inode_info *)inode->i_private;
	const struct ext2_inode *raw = &ei->raw_inode;
	uint32_t total = 0;

	for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++)
		total +=
			ext2_count_tree_blocks(inode->i_sb, raw->i_block[i], 0);
	total += ext2_count_tree_blocks(inode->i_sb,
					raw->i_block[EXT2_IND_BLOCK], 1);
	total += ext2_count_tree_blocks(inode->i_sb,
					raw->i_block[EXT2_DIND_BLOCK], 2);
	total += ext2_count_tree_blocks(inode->i_sb,
					raw->i_block[EXT2_TIND_BLOCK], 3);
	return total;
}

/* Composite: block-tree allocation serializes per leaf (ext2_alloc_block
 * and ext2_write_inode self-lock).  The in-memory i_block[] mirrors are
 * not individually serialized, matching the same-sb composite-ops policy. */
int ext2_bmap(struct inode *inode, uint32_t block, bool create,
	      uint32_t *mapped)
{
	struct ext2_inode *raw;
	uint32_t ptrs = BLOCK_SIZE / sizeof(uint32_t);
	uint32_t first;
	uint32_t second;
	struct pgcache *page;
	uint32_t *blocks;
	int ret;

	if (!mapped)
		return -EINVAL;
	*mapped = 0;
	if (!inode || !inode->i_private)
		return -EINVAL;

	raw = &EXT2_I(inode)->raw_inode;
	if (block < EXT2_NDIR_BLOCKS) {
		if (!raw->i_block[block] && create) {
			raw->i_block[block] = ext2_alloc_bmap_block(inode);
			if (raw->i_block[block]) {
				ret = ext2_write_inode(inode);
				if (ret < 0)
					return ret;
			}
		}
		*mapped = raw->i_block[block];
		return 0;
	}

	block -= EXT2_NDIR_BLOCKS;
	if (block < ptrs) {
		if (!raw->i_block[EXT2_IND_BLOCK] && create) {
			raw->i_block[EXT2_IND_BLOCK] =
				ext2_alloc_bmap_block(inode);
			if (raw->i_block[EXT2_IND_BLOCK]) {
				ret = ext2_write_inode(inode);
				if (ret < 0)
					return ret;
			}
		}
		return ext2_ind_bmap(inode, raw->i_block[EXT2_IND_BLOCK], block,
				     create, mapped);
	}

	block -= ptrs;
	if (block >= ptrs * ptrs)
		return 0;

	if (!raw->i_block[EXT2_DIND_BLOCK] && create) {
		raw->i_block[EXT2_DIND_BLOCK] = ext2_alloc_bmap_block(inode);
		if (raw->i_block[EXT2_DIND_BLOCK]) {
			ret = ext2_write_inode(inode);
			if (ret < 0)
				return ret;
		}
	}
	if (!raw->i_block[EXT2_DIND_BLOCK])
		return 0;

	first = block / ptrs;
	second = block % ptrs;
	page = pgcache_get_block(inode->i_sb->s_dev,
				    raw->i_block[EXT2_DIND_BLOCK]);
	if (!page)
		return -EIO;

	blocks = ext2_block_words(page);
	if (!blocks[first] && create) {
		blocks[first] = ext2_alloc_bmap_block(inode);
		if (blocks[first]) {
			ret = ext2_sync_metadata_page(page);
			if (ret < 0) {
				pgcache_put_page(page);
				return ret;
			}
			ret = ext2_write_inode(inode);
			if (ret < 0) {
				pgcache_put_page(page);
				return ret;
			}
		}
	}
	first = blocks[first];
	pgcache_put_page(page);

	return ext2_ind_bmap(inode, first, second, create, mapped);
}

/* Lockless block-map read: raw inode mirrors and indirect blocks are read
 * through the page cache, and the map is a best-effort snapshot taken
 * before s_lock.  Directory scans pin their pages first and serialize the
 * scan itself with s_lock; remaining callers accept a stale map, matching
 * Linux's concurrent-truncate semantics. */
uint32_t ext2_bmap_readonly(struct inode *inode, uint32_t block)
{
	struct ext2_inode *raw;
	uint32_t ptrs = BLOCK_SIZE / sizeof(uint32_t);
	uint32_t first;
	uint32_t second;

	if (!inode || !inode->i_private)
		return 0;

	raw = &EXT2_I(inode)->raw_inode;
	if (block < EXT2_NDIR_BLOCKS)
		return raw->i_block[block];

	block -= EXT2_NDIR_BLOCKS;
	if (block < ptrs)
		return ext2_ind_bmap_readonly(
			inode->i_sb, raw->i_block[EXT2_IND_BLOCK], block);

	block -= ptrs;
	if (block >= ptrs * ptrs)
		return 0;
	if (!raw->i_block[EXT2_DIND_BLOCK])
		return 0;

	first = block / ptrs;
	second = block % ptrs;
	first = ext2_ind_bmap_readonly(inode->i_sb,
				       raw->i_block[EXT2_DIND_BLOCK], first);
	if (!first)
		return 0;

	return ext2_ind_bmap_readonly(inode->i_sb, first, second);
}

int ext2_truncate_inode(struct inode *inode, uint64_t size)
{
	struct ext2_inode *raw;
	uint64_t old_size;
	uint32_t keep_blocks;
	uint32_t remaining;
	int ret;

	if (!inode || !inode->i_private)
		return -EINVAL;
	if (size > UINT32_MAX)
		return -EINVAL;
	if (size == inode->i_size)
		return 0;

	raw = &EXT2_I(inode)->raw_inode;
	old_size = inode->i_size;
	if (size == 0) {
		pgcache_invalidate_inode(inode);
		ext2_free_inode_blocks(inode);
		return ext2_write_inode(inode);
	}

	if (size < inode->i_size) {
		pgcache_truncate_inode(inode, size);
		ret = ext2_zero_truncate_tail(inode, size);
		if (ret < 0)
			return ret;

		keep_blocks = (uint32_t)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
		remaining = keep_blocks;

		for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
			uint32_t child_keep = remaining ? 1 : 0;

			ret = ext2_truncate_branch(inode, i, 0, child_keep);
			if (ret < 0)
				return ret;
			if (remaining)
				remaining--;
		}

		ret = ext2_truncate_branch(inode, EXT2_IND_BLOCK, 1, remaining);
		if (ret < 0)
			return ret;
		if (remaining > ext2_branch_span(1))
			remaining -= ext2_branch_span(1);
		else
			remaining = 0;

		ret = ext2_truncate_branch(inode, EXT2_DIND_BLOCK, 2,
					   remaining);
		if (ret < 0)
			return ret;
		if (remaining > ext2_branch_span(2))
			remaining -= ext2_branch_span(2);
		else
			remaining = 0;

		ret = ext2_truncate_branch(inode, EXT2_TIND_BLOCK, 3,
					   remaining);
		if (ret < 0)
			return ret;
	} else {
		ret = ext2_zero_extend_tail(inode, old_size);
		if (ret < 0)
			return ret;
	}

	inode->i_size = size;
	raw->i_blocks =
		ext2_inode_tree_blocks(inode) * (BLOCK_SIZE / SECTOR_SIZE);
	inode->i_blocks = raw->i_blocks;

	return ext2_write_inode(inode);
}

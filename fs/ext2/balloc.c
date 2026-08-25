#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/string.h>
#include <nuvix/printk.h>

#include "ext2.h"

#define EXT2_SYNC_RETRIES 8

static bool ext2_bitmap_test_bit(uint8_t *bitmap, uint32_t bit)
{
	return !!(bitmap[bit / 8] & (uint8_t)(1u << (bit % 8)));
}

static void ext2_bitmap_set_bit(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void ext2_bitmap_clear_bit(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static bool ext2_metadata_block_valid(const struct super_block *sb,
				      uint64_t block)
{
	const struct ext2_sb_info *sbi;

	if (!sb)
		return false;
	sbi = sb->s_private;
	return sbi && block < sbi->s_es.s_blocks_count &&
	       block < sbi->s_device_blocks &&
	       blkdev_block_valid(sb->s_dev, block);
}

static int ext2_group_bitmap(struct ext2_sb_info *sbi, uint32_t group,
			     bool inode_bitmap, uint64_t *first,
			     uint32_t *blocks, uint32_t *inodes,
			     uint32_t *bitmap)
{
	const struct ext2_group_desc *gd;
	uint32_t limit;
	int ret;

	if (!sbi || !sbi->s_group_desc || !first || !blocks || !inodes ||
	    !bitmap)
		return -EIO;
	ret = ext2_group_geometry(sbi, group, first, blocks, inodes);
	if (ret < 0)
		return ret;
	limit = inode_bitmap ? *inodes : *blocks;
	if (!limit || limit > BLOCK_SIZE * 8U)
		return -EIO;

	gd = &sbi->s_group_desc[group];
	*bitmap = inode_bitmap ? gd->bg_inode_bitmap : gd->bg_block_bitmap;
	if (ext2_require_data_block(sbi, *bitmap) || *bitmap < *first ||
	    (uint64_t)*bitmap - *first >= *blocks)
		return -EIO;
	return 0;
}

static int ext2_sync_page_retry(struct pgcache *page)
{
	int ret;

	for (uint32_t attempt = 0; attempt < EXT2_SYNC_RETRIES; attempt++) {
		ret = pgcache_sync_page(page);
		if (ret != -EBUSY)
			return ret;
	}

	pr_err("ext2: page %p busy after %u sync attempts\n", page,
	       EXT2_SYNC_RETRIES);
	return -EBUSY;
}

static int ext2_write_super_snapshot(struct super_block *sb,
				     const struct ext2_super_block *snap)
{
	uint32_t super_block = ext2_super_blocknr(BLOCK_SIZE);
	uint32_t super_off = ext2_super_offset(BLOCK_SIZE);
	struct pgcache *page;
	int ret;

	if (!sb || !snap || !ext2_metadata_block_valid(sb, super_block) ||
	    super_off > BLOCK_SIZE || sizeof(*snap) > BLOCK_SIZE - super_off)
		return -EIO;
	page = pgcache_get_block(sb->s_dev, super_block);
	if (!page)
		return -EIO;

	memcpy(page_cache_data(page) + super_off, snap, sizeof(*snap));
	ret = ext2_sync_page_retry(page);
	pgcache_put_page(page);
	return ret;
}

static int ext2_write_group_desc_snapshot(struct super_block *sb,
					  uint32_t group,
					  const struct ext2_group_desc *snap)
{
	struct ext2_sb_info *sbi;
	uint32_t desc_per_block = BLOCK_SIZE / sizeof(struct ext2_group_desc);
	uint64_t bgdt_first;
	uint64_t bgdt_blocks;
	uint64_t block;
	uint32_t offset;
	struct pgcache *page;
	int ret;

	if (!sb || !snap || !desc_per_block)
		return -EIO;
	sbi = EXT2_SB(sb);
	if (!sbi || group >= sbi->s_groups_count)
		return -EIO;
	bgdt_first = (uint64_t)sbi->s_first_data_block + 1;
	bgdt_blocks = sbi->s_groups_count / desc_per_block;
	if (sbi->s_groups_count % desc_per_block)
		bgdt_blocks++;
	if (check_add_overflow(bgdt_first, (uint64_t)(group / desc_per_block),
			       &block) ||
	    block < bgdt_first || block - bgdt_first >= bgdt_blocks ||
	    !ext2_metadata_block_valid(sb, block))
		return -EIO;
	offset = (group % desc_per_block) * sizeof(struct ext2_group_desc);
	if (offset > BLOCK_SIZE || sizeof(*snap) > BLOCK_SIZE - offset)
		return -EIO;

	page = pgcache_get_block(sb->s_dev, block);
	if (!page)
		return -EIO;

	memcpy(page_cache_data(page) + offset, snap, sizeof(*snap));
	ret = ext2_sync_page_retry(page);
	pgcache_put_page(page);
	return ret;
}

static int ext2_zero_block(struct super_block *sb, uint32_t block)
{
	struct pgcache *page;
	int ret;

	if (!sb || !ext2_data_block_valid(EXT2_SB(sb), block))
		return -EIO;
	page = pgcache_get_block(sb->s_dev, block);
	if (!page)
		return -EIO;

	memset(page_cache_data(page), 0, BLOCK_SIZE);
	ret = ext2_sync_page_retry(page);
	pgcache_put_page(page);
	return ret;
}

/* Allocation/free leaves pin the bitmap page before taking s_lock (page
 * fetches may allocate and are therefore forbidden under a spinlock), then
 * scan/set the bitmap and update the in-memory counters under the lock.
 * Counter snapshots are copied under the lock; syncs run after unlocking
 * and retry -EBUSY so a concurrent writeback cannot lose the update. */
uint32_t ext2_alloc_block(struct inode *inode)
{
	struct super_block *sb;
	struct ext2_sb_info *sbi;
	struct ext2_super_block es_snap;
	struct ext2_group_desc gd_snap;
	uint64_t total_blocks;
	uint32_t preferred = 0;

	if (!inode || !inode->i_sb)
		return 0;
	sb = inode->i_sb;
	sbi = EXT2_SB(sb);
	if (!sbi || !sbi->s_group_desc || !sbi->s_groups_count ||
	    !sbi->s_blocks_per_group || !sbi->s_inodes_per_group ||
	    sbi->s_es.s_blocks_count <= sbi->s_first_data_block)
		return 0;
	total_blocks = sbi->s_es.s_blocks_count - sbi->s_first_data_block;
	if (inode->i_ino > 0 && inode->i_ino <= UINT32_MAX)
		preferred =
			(uint32_t)(inode->i_ino - 1) / sbi->s_inodes_per_group;
	preferred %= sbi->s_groups_count;

	for (uint32_t pass = 0; pass < sbi->s_groups_count; pass++) {
		uint32_t group = (preferred + pass) % sbi->s_groups_count;
		struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		struct pgcache *page;
		uint64_t group_first;
		uint32_t group_blocks;
		uint32_t group_inodes;
		uint32_t bitmap_block;
		uint8_t *data;
		uint32_t block = 0;
		int ret;

		ret = ext2_group_bitmap(sbi, group, false, &group_first,
					&group_blocks, &group_inodes,
					&bitmap_block);
		if (ret < 0)
			return 0;
		(void)group_inodes;
		if (gd->bg_free_blocks_count > group_blocks)
			return 0;
		if (!gd->bg_free_blocks_count)
			continue;

		page = pgcache_get_block(sb->s_dev, bitmap_block);
		if (!page)
			return 0;
		data = page_cache_data(page);

		spin_lock(&sbi->s_lock);
		if (gd->bg_block_bitmap == bitmap_block &&
		    gd->bg_free_blocks_count &&
		    gd->bg_free_blocks_count <= group_blocks &&
		    sbi->s_es.s_free_blocks_count &&
		    sbi->s_es.s_free_blocks_count <= total_blocks) {
			for (uint32_t bit = 0; bit < group_blocks; bit++) {
				uint64_t candidate;

				if (ext2_bitmap_test_bit(data, bit))
					continue;
				if (check_add_overflow(group_first,
						       (uint64_t)bit,
						       &candidate) ||
				    !ext2_data_block_valid(sbi, candidate))
					continue;
				block = (uint32_t)candidate;
				ext2_bitmap_set_bit(data, bit);
				gd->bg_free_blocks_count--;
				sbi->s_es.s_free_blocks_count--;
				break;
			}
		}
		es_snap = sbi->s_es;
		gd_snap = sbi->s_group_desc[group];
		spin_unlock(&sbi->s_lock);

		if (block) {
			int sync_ret;

			sync_ret = ext2_sync_page_retry(page);
			if (sync_ret < 0)
				pr_err("ext2: failed to sync block bitmap: "
				       "%d\n",
				       sync_ret);
			pgcache_put_page(page);
			ret = ext2_write_group_desc_snapshot(sb, group,
							     &gd_snap);
			if (ret < 0)
				pr_err("ext2: failed to write group "
				       "descriptor: %d\n",
				       ret);
			ret = ext2_write_super_snapshot(sb, &es_snap);
			if (ret < 0)
				pr_err("ext2: failed to write superblock: %d\n",
				       ret);
			ret = ext2_zero_block(sb, block);
			if (ret < 0) {
				pr_err("ext2: failed to zero allocated block "
				       "%u: %d\n",
				       block, ret);
				ext2_free_block(sb, block);
				return 0;
			}
			return block;
		}

		pgcache_put_page(page);
	}

	return 0;
}

void ext2_free_block(struct super_block *sb, uint32_t block)
{
	struct ext2_sb_info *sbi;
	struct ext2_super_block es_snap;
	struct ext2_group_desc gd_snap;
	uint64_t total_blocks;
	uint64_t group_first;
	uint32_t group_blocks;
	uint32_t group_inodes;
	uint32_t bitmap_block;
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	uint8_t *data;
	bool cleared = false;
	int ret;

	if (!sb)
		return;
	sbi = EXT2_SB(sb);
	if (!sbi || !sbi->s_group_desc || !sbi->s_groups_count ||
	    !sbi->s_blocks_per_group ||
	    sbi->s_es.s_blocks_count <= sbi->s_first_data_block)
		return;
	total_blocks = sbi->s_es.s_blocks_count - sbi->s_first_data_block;
	if (block < sbi->s_first_data_block ||
	    !ext2_data_block_valid(sbi, block) ||
	    block >= sbi->s_es.s_blocks_count)
		return;

	group = (block - sbi->s_first_data_block) / sbi->s_blocks_per_group;
	if (group >= sbi->s_groups_count)
		return;
	ret = ext2_group_bitmap(sbi, group, false, &group_first, &group_blocks,
				&group_inodes, &bitmap_block);
	if (ret < 0 || (uint64_t)block < group_first ||
	    (uint64_t)block - group_first >= group_blocks)
		return;
	(void)group_inodes;
	bit = (uint32_t)((uint64_t)block - group_first);

	page = pgcache_get_block(sb->s_dev, bitmap_block);
	if (!page)
		return;
	data = page_cache_data(page);

	spin_lock(&sbi->s_lock);
	if (sbi->s_group_desc[group].bg_block_bitmap == bitmap_block &&
	    sbi->s_group_desc[group].bg_free_blocks_count < group_blocks &&
	    sbi->s_es.s_free_blocks_count < total_blocks &&
	    ext2_bitmap_test_bit(data, bit)) {
		ext2_bitmap_clear_bit(data, bit);
		sbi->s_group_desc[group].bg_free_blocks_count++;
		sbi->s_es.s_free_blocks_count++;
		cleared = true;
	}
	es_snap = sbi->s_es;
	gd_snap = sbi->s_group_desc[group];
	spin_unlock(&sbi->s_lock);

	if (cleared) {
		int sync_ret;

		sync_ret = ext2_sync_page_retry(page);
		if (sync_ret < 0)
			pr_err("ext2: failed to sync block bitmap: %d\n",
			       sync_ret);
		ret = ext2_write_group_desc_snapshot(sb, group, &gd_snap);
		if (ret < 0)
			pr_err("ext2: failed to write group descriptor: %d\n",
			       ret);
		ret = ext2_write_super_snapshot(sb, &es_snap);
		if (ret < 0)
			pr_err("ext2: failed to write superblock: %d\n", ret);
	}

	pgcache_put_page(page);
}

uint32_t ext2_alloc_inode(struct super_block *sb, uint16_t mode)
{
	struct ext2_sb_info *sbi;

	if (!sb)
		return 0;
	sbi = EXT2_SB(sb);
	if (!sbi || !sbi->s_group_desc || !sbi->s_groups_count ||
	    !sbi->s_inodes_per_group || !sbi->s_es.s_inodes_count)
		return 0;

	for (uint32_t group = 0; group < sbi->s_groups_count; group++) {
		struct ext2_super_block es_snap;
		struct ext2_group_desc gd_snap;
		struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		struct pgcache *page;
		uint64_t group_first;
		uint32_t group_blocks;
		uint32_t group_inodes;
		uint32_t bitmap_block;
		uint8_t *data;
		uint32_t ino = 0;
		int ret;

		ret = ext2_group_bitmap(sbi, group, true, &group_first,
					&group_blocks, &group_inodes,
					&bitmap_block);
		if (ret < 0)
			return 0;
		(void)group_first;
		(void)group_blocks;
		if (gd->bg_free_inodes_count > group_inodes)
			return 0;
		if (!gd->bg_free_inodes_count)
			continue;

		page = pgcache_get_block(sb->s_dev, bitmap_block);
		if (!page)
			return 0;
		data = page_cache_data(page);

		spin_lock(&sbi->s_lock);
		if (gd->bg_inode_bitmap == bitmap_block &&
		    gd->bg_free_inodes_count &&
		    gd->bg_free_inodes_count <= group_inodes &&
		    sbi->s_es.s_free_inodes_count &&
		    sbi->s_es.s_free_inodes_count <= sbi->s_es.s_inodes_count &&
		    gd->bg_used_dirs_count <= group_inodes) {
			for (uint32_t bit = 0; bit < group_inodes; bit++) {
				uint64_t ino64;

				if (ext2_bitmap_test_bit(data, bit))
					continue;
				ino64 = (uint64_t)group *
						sbi->s_inodes_per_group +
					bit + 1;
				if (!ino64 ||
				    ino64 > sbi->s_es.s_inodes_count ||
				    ino64 > UINT32_MAX)
					continue;
				if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR &&
				    gd->bg_used_dirs_count >= UINT16_MAX)
					break;
				ino = (uint32_t)ino64;
				ext2_bitmap_set_bit(data, bit);
				gd->bg_free_inodes_count--;
				if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
					gd->bg_used_dirs_count++;
				sbi->s_es.s_free_inodes_count--;
				break;
			}
		}
		es_snap = sbi->s_es;
		gd_snap = sbi->s_group_desc[group];
		spin_unlock(&sbi->s_lock);

		if (ino) {
			int sync_ret;

			sync_ret = ext2_sync_page_retry(page);
			if (sync_ret < 0)
				pr_err("ext2: failed to sync inode bitmap: "
				       "%d\n",
				       sync_ret);
			pgcache_put_page(page);
			ret = ext2_write_group_desc_snapshot(sb, group,
							     &gd_snap);
			if (ret < 0)
				pr_err("ext2: failed to write group "
				       "descriptor: %d\n",
				       ret);
			ret = ext2_write_super_snapshot(sb, &es_snap);
			if (ret < 0)
				pr_err("ext2: failed to write superblock: %d\n",
				       ret);
			return ino;
		}

		pgcache_put_page(page);
	}

	return 0;
}

void ext2_free_inode(struct super_block *sb, uint32_t ino)
{
	struct ext2_sb_info *sbi;
	struct ext2_super_block es_snap;
	struct ext2_group_desc gd_snap;
	uint64_t group_first;
	uint32_t group_blocks;
	uint32_t group_inodes;
	uint32_t bitmap_block;
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	uint8_t *data;
	bool cleared = false;
	int ret;

	if (!sb)
		return;
	sbi = EXT2_SB(sb);
	if (!sbi || !sbi->s_group_desc || !sbi->s_groups_count ||
	    !sbi->s_inodes_per_group || !ino || ino > sbi->s_es.s_inodes_count)
		return;

	group = (ino - 1) / sbi->s_inodes_per_group;
	bit = (ino - 1) % sbi->s_inodes_per_group;
	if (group >= sbi->s_groups_count)
		return;
	ret = ext2_group_bitmap(sbi, group, true, &group_first, &group_blocks,
				&group_inodes, &bitmap_block);
	if (ret < 0 || bit >= group_inodes)
		return;
	(void)group_first;
	(void)group_blocks;

	page = pgcache_get_block(sb->s_dev, bitmap_block);
	if (!page)
		return;
	data = page_cache_data(page);

	spin_lock(&sbi->s_lock);
	if (sbi->s_group_desc[group].bg_inode_bitmap == bitmap_block &&
	    sbi->s_group_desc[group].bg_free_inodes_count < group_inodes &&
	    sbi->s_es.s_free_inodes_count < sbi->s_es.s_inodes_count &&
	    ext2_bitmap_test_bit(data, bit)) {
		ext2_bitmap_clear_bit(data, bit);
		sbi->s_group_desc[group].bg_free_inodes_count++;
		sbi->s_es.s_free_inodes_count++;
		cleared = true;
	}
	es_snap = sbi->s_es;
	gd_snap = sbi->s_group_desc[group];
	spin_unlock(&sbi->s_lock);

	if (cleared) {
		int sync_ret;

		sync_ret = ext2_sync_page_retry(page);
		if (sync_ret < 0)
			pr_err("ext2: failed to sync inode bitmap: %d\n",
			       sync_ret);
		ret = ext2_write_group_desc_snapshot(sb, group, &gd_snap);
		if (ret < 0)
			pr_err("ext2: failed to write group descriptor: %d\n",
			       ret);
		ret = ext2_write_super_snapshot(sb, &es_snap);
		if (ret < 0)
			pr_err("ext2: failed to write superblock: %d\n", ret);
	}

	pgcache_put_page(page);
}

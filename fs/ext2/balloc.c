#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/printk.h>

#include "ext2.h"

#define EXT2_SYNC_RETRIES 8

static bool bitmap_test_bit(uint8_t *bitmap, uint32_t bit)
{
	return !!(bitmap[bit / 8] & (uint8_t)(1u << (bit % 8)));
}

static void bitmap_set_bit(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bitmap_clear_bit(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static uint32_t ext2_group_first_block(struct ext2_sb_info *sbi,
						       uint32_t group)
{
	return sbi->s_first_data_block + group * sbi->s_blocks_per_group;
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
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	uint32_t desc_per_block = BLOCK_SIZE / sizeof(struct ext2_group_desc);
	uint32_t block = EXT2_BGDT_BLOCK(sbi->s_first_data_block) +
			 group / desc_per_block;
	uint32_t offset =
		(group % desc_per_block) * sizeof(struct ext2_group_desc);
	struct pgcache *page = pgcache_get_block(sb->s_dev, block);
	int ret;

	if (!page)
		return -EIO;

	memcpy(page_cache_data(page) + offset, snap, sizeof(*snap));
	ret = ext2_sync_page_retry(page);
	pgcache_put_page(page);
	return ret;
}

static uint32_t ext2_group_blocks(struct ext2_sb_info *sbi, uint32_t group)
{
	uint32_t first = ext2_group_first_block(sbi, group);
	uint32_t blocks = sbi->s_es.s_blocks_count - first;

	if (blocks > sbi->s_blocks_per_group)
		blocks = sbi->s_blocks_per_group;
	return blocks;
}

static void ext2_zero_block(struct super_block *sb, uint32_t block)
{
	struct pgcache *page = pgcache_get_block(sb->s_dev, block);
	int sync_ret;

	if (!page)
		return;

	memset(page_cache_data(page), 0, BLOCK_SIZE);
	sync_ret = pgcache_sync_page(page);
	(void)sync_ret;
	pgcache_put_page(page);
}

/* Allocation/free leaves pin the bitmap page before taking s_lock (page
 * fetches may allocate and are therefore forbidden under a spinlock), then
 * scan/set the bitmap and update the in-memory counters under the lock.
 * Counter snapshots are copied under the lock; syncs run after unlocking
 * and retry -EBUSY so a concurrent writeback cannot lose the update. */
uint32_t ext2_alloc_block(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block es_snap;
	struct ext2_group_desc gd_snap;
	uint32_t preferred = 0;

	if (inode->i_ino > 0)
		preferred =
			(uint32_t)(inode->i_ino - 1) / sbi->s_inodes_per_group;

	for (uint32_t pass = 0; pass < sbi->s_groups_count; pass++) {
		uint32_t group = (preferred + pass) % sbi->s_groups_count;
		struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		struct pgcache *page;
		uint32_t group_blocks;
		uint8_t *data;
		uint32_t block = 0;

		if (!gd->bg_free_blocks_count)
			continue;

		page = pgcache_get_block(sb->s_dev, gd->bg_block_bitmap);
		if (!page)
			return 0;
		data = page_cache_data(page);

		spin_lock(&sbi->s_lock);
		if (gd->bg_free_blocks_count) {
			group_blocks = ext2_group_blocks(sbi, group);
			for (uint32_t bit = 0; bit < group_blocks; bit++) {
				if (bitmap_test_bit(data, bit))
					continue;

				block =
					ext2_group_first_block(sbi, group) + bit;
				bitmap_set_bit(data, bit);
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
			(void)sync_ret;
			pgcache_put_page(page);
			ext2_write_group_desc_snapshot(sb, group, &gd_snap);
			ext2_write_super_snapshot(sb, &es_snap);
			ext2_zero_block(sb, block);
			return block;
		}

		pgcache_put_page(page);
	}

	return 0;
}

void ext2_free_block(struct super_block *sb, uint32_t block)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block es_snap;
	struct ext2_group_desc gd_snap;
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	uint8_t *data;
	bool cleared = false;

	if (block < sbi->s_first_data_block ||
	    block >= sbi->s_es.s_blocks_count)
		return;

	group = (block - sbi->s_first_data_block) / sbi->s_blocks_per_group;
	bit = block - ext2_group_first_block(sbi, group);
	if (group >= sbi->s_groups_count)
		return;

	page = pgcache_get_block(sb->s_dev,
				    sbi->s_group_desc[group].bg_block_bitmap);
	if (!page)
		return;
	data = page_cache_data(page);

	spin_lock(&sbi->s_lock);
	if (bitmap_test_bit(data, bit)) {
		bitmap_clear_bit(data, bit);
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
		(void)sync_ret;
		ext2_write_group_desc_snapshot(sb, group, &gd_snap);
		ext2_write_super_snapshot(sb, &es_snap);
	}

	pgcache_put_page(page);
}

uint32_t ext2_alloc_inode(struct super_block *sb, uint16_t mode)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	for (uint32_t group = 0; group < sbi->s_groups_count; group++) {
		struct ext2_super_block es_snap;
		struct ext2_group_desc gd_snap;
		struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		struct pgcache *page;
		uint8_t *data;
		uint32_t ino = 0;

		if (!gd->bg_free_inodes_count)
			continue;

		page = pgcache_get_block(sb->s_dev, gd->bg_inode_bitmap);
		if (!page)
			return 0;
		data = page_cache_data(page);

		spin_lock(&sbi->s_lock);
		if (gd->bg_free_inodes_count) {
			for (uint32_t bit = 0;
			     bit < sbi->s_inodes_per_group; bit++) {
				if (bitmap_test_bit(data, bit))
					continue;

				ino = group * sbi->s_inodes_per_group + bit + 1;
				bitmap_set_bit(data, bit);
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
			(void)sync_ret;
			pgcache_put_page(page);
			ext2_write_group_desc_snapshot(sb, group, &gd_snap);
			ext2_write_super_snapshot(sb, &es_snap);
			return ino;
		}

		pgcache_put_page(page);
	}

	return 0;
}

void ext2_free_inode(struct super_block *sb, uint32_t ino)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block es_snap;
	struct ext2_group_desc gd_snap;
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	uint8_t *data;
	bool cleared = false;

	if (!ino || ino > sbi->s_es.s_inodes_count)
		return;

	group = (ino - 1) / sbi->s_inodes_per_group;
	bit = (ino - 1) % sbi->s_inodes_per_group;
	if (group >= sbi->s_groups_count)
		return;

	page = pgcache_get_block(sb->s_dev,
				    sbi->s_group_desc[group].bg_inode_bitmap);
	if (!page)
		return;
	data = page_cache_data(page);

	spin_lock(&sbi->s_lock);
	if (bitmap_test_bit(data, bit)) {
		bitmap_clear_bit(data, bit);
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
		(void)sync_ret;
		ext2_write_group_desc_snapshot(sb, group, &gd_snap);
		ext2_write_super_snapshot(sb, &es_snap);
	}

	pgcache_put_page(page);
}

#include <nuvix/errno.h>
#include <nuvix/page_cache.h>

#include "ext2.h"

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

static int ext2_sync_super(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	uint32_t super_block = ext2_super_blocknr(BLOCK_SIZE);
	uint32_t super_off = ext2_super_offset(BLOCK_SIZE);
	struct pgcache *page = pgcache_get_block(sb->s_dev, super_block);
	int ret;

	if (!page)
		return -EIO;

	memcpy(page_cache_data(page) + super_off, &sbi->s_es,
	       sizeof(sbi->s_es));
	ret = pgcache_sync_page(page);
	pgcache_put_page(page);
	return ret;
}

static int ext2_sync_group_desc(struct super_block *sb, uint32_t group)
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

	memcpy(page_cache_data(page) + offset, &sbi->s_group_desc[group],
	       sizeof(struct ext2_group_desc));
	ret = pgcache_sync_page(page);
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

uint32_t ext2_alloc_block(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
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

		if (!gd->bg_free_blocks_count)
			continue;

		page = pgcache_get_block(sb->s_dev, gd->bg_block_bitmap);
		if (!page)
			return 0;
		data = page_cache_data(page);

		group_blocks = ext2_group_blocks(sbi, group);
		for (uint32_t bit = 0; bit < group_blocks; bit++) {
			if (bitmap_test_bit(data, bit))
				continue;

			uint32_t block =
				ext2_group_first_block(sbi, group) + bit;
			int sync_ret;

			bitmap_set_bit(data, bit);
			sync_ret = pgcache_sync_page(page);
			(void)sync_ret;
			pgcache_put_page(page);

			gd->bg_free_blocks_count--;
			sbi->s_es.s_free_blocks_count--;
			ext2_sync_group_desc(sb, group);
			ext2_sync_super(sb);
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
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	uint8_t *data;
	int sync_ret;

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

	if (bitmap_test_bit(data, bit)) {
		bitmap_clear_bit(data, bit);
		sync_ret = pgcache_sync_page(page);
		(void)sync_ret;
		sbi->s_group_desc[group].bg_free_blocks_count++;
		sbi->s_es.s_free_blocks_count++;
		ext2_sync_group_desc(sb, group);
		ext2_sync_super(sb);
	}

	pgcache_put_page(page);
}

uint32_t ext2_alloc_inode(struct super_block *sb, uint16_t mode)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	for (uint32_t group = 0; group < sbi->s_groups_count; group++) {
		struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		struct pgcache *page;
		uint8_t *data;

		if (!gd->bg_free_inodes_count)
			continue;

		page = pgcache_get_block(sb->s_dev, gd->bg_inode_bitmap);
		if (!page)
			return 0;
		data = page_cache_data(page);

		for (uint32_t bit = 0; bit < sbi->s_inodes_per_group; bit++) {
			if (bitmap_test_bit(data, bit))
				continue;

			uint32_t ino =
				group * sbi->s_inodes_per_group + bit + 1;
			int sync_ret;

			bitmap_set_bit(data, bit);
			sync_ret = pgcache_sync_page(page);
			(void)sync_ret;
			pgcache_put_page(page);

			gd->bg_free_inodes_count--;
			if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
				gd->bg_used_dirs_count++;
			sbi->s_es.s_free_inodes_count--;
			ext2_sync_group_desc(sb, group);
			ext2_sync_super(sb);
			return ino;
		}

		pgcache_put_page(page);
	}

	return 0;
}

void ext2_free_inode(struct super_block *sb, uint32_t ino)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	uint8_t *data;
	int sync_ret;

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

	if (bitmap_test_bit(data, bit)) {
		bitmap_clear_bit(data, bit);
		sync_ret = pgcache_sync_page(page);
		(void)sync_ret;
		sbi->s_group_desc[group].bg_free_inodes_count++;
		sbi->s_es.s_free_inodes_count++;
		ext2_sync_group_desc(sb, group);
		ext2_sync_super(sb);
	}

	pgcache_put_page(page);
}

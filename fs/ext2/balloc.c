#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/string.h>
#include <nuvix/printk.h>

#include "ext2.h"

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

/* Pin all counter pages before changing a bitmap: after this succeeds,
 * publishing the matching counters cannot fail or allocate under s_lock. */
struct ext2_alloc_pages {
	struct pgcache *super;
	struct pgcache *group;
};

static int ext2_get_alloc_pages(struct super_block *sb, uint32_t group,
				struct ext2_alloc_pages *pages)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	uint32_t desc_per_block = BLOCK_SIZE / sizeof(struct ext2_group_desc);
	uint64_t block = (uint64_t)sbi->s_first_data_block + 1 +
			 group / desc_per_block;

	if (group >= sbi->s_groups_count ||
	    !ext2_metadata_block_valid(sb, block))
		return -EIO;
	pages->super = pgcache_get_block(sb->s_dev,
					 ext2_super_blocknr(BLOCK_SIZE));
	if (!pages->super)
		return -EIO;
	pages->group = pgcache_get_block(sb->s_dev, block);
	if (!pages->group) {
		pgcache_put_page(pages->super);
		return -EIO;
	}
	return 0;
}

static void ext2_put_alloc_pages(struct ext2_alloc_pages *pages)
{
	pgcache_put_page(pages->group);
	pgcache_put_page(pages->super);
}

/* Caller holds s_lock across bitmap/counter changes and this copy. */
static void ext2_dirty_alloc_pages(struct ext2_sb_info *sbi, uint32_t group,
				  struct ext2_alloc_pages *pages,
				  struct pgcache *bitmap)
{
	uint32_t desc_per_block = BLOCK_SIZE / sizeof(struct ext2_group_desc);
	uint32_t offset = (group % desc_per_block) * sizeof(struct ext2_group_desc);

	memcpy(page_cache_data(pages->super) + ext2_super_offset(BLOCK_SIZE),
	       &sbi->s_es, sizeof(sbi->s_es));
	memcpy(page_cache_data(pages->group) + offset, &sbi->s_group_desc[group],
	       sizeof(struct ext2_group_desc));
	pgcache_mark_dirty(bitmap);
	pgcache_mark_dirty(pages->group);
	pgcache_mark_dirty(pages->super);
}

static int ext2_zero_block(struct super_block *sb, uint32_t block)
{
	struct pgcache *page;

	if (!sb || !ext2_data_block_valid(EXT2_SB(sb), block))
		return -EIO;
	page = pgcache_get(sb->s_dev, block, PAGE_CACHE_CREATE, NULL);
	if (!page)
		return -EIO;

	memset(page_cache_data(page), 0, BLOCK_SIZE);
	pgcache_mark_dirty(page);
	pgcache_put_page(page);
	return 0;
}

/* Allocation/free updates the bitmap and cached counters under s_lock.
 * The page-cache queue owns writeback; mutations never force device I/O. */
uint32_t ext2_alloc_block(struct inode *inode)
{
	struct super_block *sb;
	struct ext2_sb_info *sbi;
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
		struct ext2_alloc_pages counter_pages;
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
		ret = ext2_get_alloc_pages(sb, group, &counter_pages);
		if (ret < 0) {
			pgcache_put_page(page);
			return 0;
		}
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

		if (block)
			ext2_dirty_alloc_pages(sbi, group, &counter_pages, page);
		spin_unlock(&sbi->s_lock);
		ext2_put_alloc_pages(&counter_pages);

		if (block) {
			pgcache_put_page(page);
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
	uint64_t total_blocks;
	uint64_t group_first;
	uint32_t group_blocks;
	uint32_t group_inodes;
	uint32_t bitmap_block;
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	struct ext2_alloc_pages counter_pages;
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
	ret = ext2_get_alloc_pages(sb, group, &counter_pages);
	if (ret < 0) {
		pgcache_put_page(page);
		sb->s_error = ret;
		return;
	}
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

	if (cleared)
		ext2_dirty_alloc_pages(sbi, group, &counter_pages, page);
	spin_unlock(&sbi->s_lock);
	ext2_put_alloc_pages(&counter_pages);

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
		struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		struct pgcache *page;
		struct ext2_alloc_pages counter_pages;
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
		ret = ext2_get_alloc_pages(sb, group, &counter_pages);
		if (ret < 0) {
			pgcache_put_page(page);
			return 0;
		}
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

		if (ino)
			ext2_dirty_alloc_pages(sbi, group, &counter_pages, page);
		spin_unlock(&sbi->s_lock);
		ext2_put_alloc_pages(&counter_pages);

		if (ino) {
			pgcache_put_page(page);
			return ino;
		}

		pgcache_put_page(page);
	}

	return 0;
}

void ext2_free_inode(struct super_block *sb, uint32_t ino, uint16_t mode)
{
	struct ext2_sb_info *sbi;
	uint64_t group_first;
	uint32_t group_blocks;
	uint32_t group_inodes;
	uint32_t bitmap_block;
	uint32_t group;
	uint32_t bit;
	struct pgcache *page;
	struct ext2_alloc_pages counter_pages;
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
	ret = ext2_get_alloc_pages(sb, group, &counter_pages);
	if (ret < 0) {
		pgcache_put_page(page);
		sb->s_error = ret;
		return;
	}
	data = page_cache_data(page);

	spin_lock(&sbi->s_lock);
	if (sbi->s_group_desc[group].bg_inode_bitmap == bitmap_block &&
	    sbi->s_group_desc[group].bg_free_inodes_count < group_inodes &&
	    sbi->s_es.s_free_inodes_count < sbi->s_es.s_inodes_count &&
	    ext2_bitmap_test_bit(data, bit)) {
		ext2_bitmap_clear_bit(data, bit);
		sbi->s_group_desc[group].bg_free_inodes_count++;
		if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR &&
		    sbi->s_group_desc[group].bg_used_dirs_count)
			sbi->s_group_desc[group].bg_used_dirs_count--;
		sbi->s_es.s_free_inodes_count++;
		cleared = true;
	}

	if (cleared)
		ext2_dirty_alloc_pages(sbi, group, &counter_pages, page);
	spin_unlock(&sbi->s_lock);
	ext2_put_alloc_pages(&counter_pages);

	pgcache_put_page(page);
}

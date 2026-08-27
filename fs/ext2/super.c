#include <nuvix/blkdev.h>
#include <nuvix/cleanup.h>
#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/statfs.h>
#include <nuvix/vmalloc.h>
#include <nuvix/vfs.h>

#include "ext2.h"

#define EXT2_FEATURE_COMPAT_SUPPORTED	 0
#define EXT2_FEATURE_INCOMPAT_SUPPORTED	 EXT2_FEATURE_INCOMPAT_FILETYPE
#define EXT2_FEATURE_RO_COMPAT_SUPPORTED EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER

static int ext2_probe(dev_t dev);
static int ext2_mount(struct file_system_type *fs_type, dev_t dev,
		      const void *data, struct super_block **out_sb);
static int ext2_evict_inode(struct inode *inode);
static void ext2_put_super(struct super_block *sb);
static int ext2_statfs(struct super_block *sb, struct statfs64 *buf);

static const struct super_operations ext2_sops = {
	.read_inode = ext2_read_inode,
	.write_inode = ext2_write_inode,
	.datasync_inode = ext2_datasync_inode,
	.evict_inode = ext2_evict_inode,
	.put_super = ext2_put_super,
	.statfs = ext2_statfs,
};

static struct file_system_type ext2_fs_type = {
	.name = "ext2",
	.probe = ext2_probe,
	.mount = ext2_mount,
};

struct ext2_geometry {
	uint64_t device_blocks;
	uint32_t groups_count;
	uint32_t inode_size;
};

static bool ext2_div_round_up_u64(uint64_t value, uint64_t divisor,
				  uint64_t *result)
{
	if (!divisor || !result)
		return false;

	*result = value / divisor;
	if (value % divisor)
		(*result)++;
	return true;
}

static bool ext2_range_valid(uint64_t first, uint64_t count, uint64_t limit)
{
	return first <= limit && count <= limit - first;
}

static int ext2_evict_inode(struct inode *inode)
{
	int ret;

	if (!inode)
		return 0;

	/* Page-cache invalidation may fail while resident PTEs or in-flight
	 * I/O pin the mapping; the inode object stays intact so retirement
	 * can be retried after the caller resolves the pins. */
	ret = pgcache_invalidate_inode(inode);
	if (ret < 0) {
		if (inode->i_sb)
			inode->i_sb->s_error = ret;
		return ret;
	}
	if (inode->i_nlink == 0 && inode->i_private) {
		ext2_free_inode_blocks(inode);
		ext2_free_inode(inode->i_sb, (uint32_t)inode->i_ino);
	}
	kfree(inode->i_private);
	inode->i_private = NULL;
	return 0;
}

static void ext2_free_sbi(struct ext2_sb_info *sbi)
{
	if (!sbi)
		return;

	if (sbi->s_group_desc)
		vfree(sbi->s_group_desc);
	kfree(sbi);
}

static void ext2_put_super(struct super_block *sb)
{
	if (!sb)
		return;

	ext2_free_sbi(EXT2_SB(sb));
}

static void ext2_free_super(struct super_block *sb)
{
	vfs_super_destroy(sb);
}

static bool ext2_uuid_is_zero(const uint8_t uuid[16])
{
	for (uint32_t i = 0; i < 16; i++) {
		if (uuid[i] != 0)
			return false;
	}

	return true;
}

static uint32_t ext2_uuid_word(const uint8_t uuid[16], uint32_t offset)
{
	return (uint32_t)uuid[offset] | ((uint32_t)uuid[offset + 1] << 8) |
	       ((uint32_t)uuid[offset + 2] << 16) |
	       ((uint32_t)uuid[offset + 3] << 24);
}

static void ext2_statfs_fsid(const struct super_block *sb,
			     const struct ext2_sb_info *sbi,
			     struct statfs64 *buf)
{
	if (!ext2_uuid_is_zero(sbi->s_es.s_uuid)) {
		uint32_t fsid0 = ext2_uuid_word(sbi->s_es.s_uuid, 0) ^
				 ext2_uuid_word(sbi->s_es.s_uuid, 8);
		uint32_t fsid1 = ext2_uuid_word(sbi->s_es.s_uuid, 4) ^
				 ext2_uuid_word(sbi->s_es.s_uuid, 12);

		buf->f_fsid[0] = (int32_t)fsid0;
		buf->f_fsid[1] = (int32_t)fsid1;
		if (buf->f_fsid[0] != 0 || buf->f_fsid[1] != 0)
			return;
	}

	buf->f_fsid[0] = (int32_t)sb->s_dev;
	buf->f_fsid[1] = 0;
}

static int ext2_statfs(struct super_block *sb, struct statfs64 *buf)
{
	struct ext2_sb_info *sbi;
	uint64_t free_blocks = 0;
	uint64_t free_inodes = 0;

	if (!sb || !buf)
		return -EINVAL;
	sbi = EXT2_SB(sb);
	if (!sbi)
		return -EINVAL;

	spin_lock(&sbi->s_lock);
	for (uint32_t i = 0; i < sbi->s_groups_count; i++) {
		free_blocks += sbi->s_group_desc[i].bg_free_blocks_count;
		free_inodes += sbi->s_group_desc[i].bg_free_inodes_count;
	}

	memset(buf, 0, sizeof(*buf));
	buf->f_type = EXT2_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = sbi->s_es.s_blocks_count;
	buf->f_bfree = free_blocks;
	buf->f_bavail = free_blocks;
	buf->f_files = sbi->s_es.s_inodes_count;
	buf->f_ffree = free_inodes;
	ext2_statfs_fsid(sb, sbi, buf);
	buf->f_namelen = EXT2_NAME_LEN;
	buf->f_frsize = sb->s_blocksize;
	buf->f_flags = 0;
	spin_unlock(&sbi->s_lock);
	return 0;
}

static int ext2_read_super_block(dev_t dev, struct ext2_super_block *es)
{
	struct pgcache *page;
	uint32_t super_block;
	uint32_t super_off;

	if (!es)
		return -EINVAL;
	if (!lookup_blkdev(dev))
		return -ENXIO;

	super_block = ext2_super_blocknr(BLOCK_SIZE);
	super_off = ext2_super_offset(BLOCK_SIZE);
	if (!blkdev_block_valid(dev, super_block) || super_off > BLOCK_SIZE ||
	    sizeof(*es) > BLOCK_SIZE - super_off)
		return -EIO;
	page = pgcache_get_block(dev, super_block);
	if (!page)
		return -EIO;

	memcpy(es, page_cache_data(page) + super_off, sizeof(*es));
	pgcache_put_page(page);
	return 0;
}

static int ext2_check_super_block(dev_t dev, const struct ext2_super_block *es,
				  struct ext2_geometry *geometry)
{
	uint64_t block_groups;
	uint64_t inode_groups;
	uint64_t bgdt_first;
	uint64_t bgdt_blocks;
	uint64_t bgdt_bytes;
	uint64_t device_blocks;
	uint32_t inode_size;

	if (!lookup_blkdev(dev))
		return -ENXIO;

	if (!es || !geometry)
		return -EINVAL;
	if (es->s_magic != EXT2_SUPER_MAGIC)
		return -ENODEV;
	if (es->s_feature_compat & ~EXT2_FEATURE_COMPAT_SUPPORTED)
		return -EINVAL;
	if ((es->s_feature_incompat & ~EXT2_FEATURE_INCOMPAT_SUPPORTED) ||
	    !(es->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE))
		return -EINVAL;
	if (es->s_feature_ro_compat & ~EXT2_FEATURE_RO_COMPAT_SUPPORTED)
		return -EINVAL;
	if (es->s_rev_level > EXT2_DYNAMIC_REV || es->s_log_block_size != 2)
		return -EINVAL;
	if (es->s_first_data_block != 0)
		return -EIO;

	inode_size = es->s_rev_level == EXT2_GOOD_OLD_REV
			     ? EXT2_GOOD_OLD_INODE_SIZE
			     : es->s_inode_size;
	if (inode_size < sizeof(struct ext2_inode) || inode_size > BLOCK_SIZE ||
	    inode_size % sizeof(uint32_t) != 0 || BLOCK_SIZE % inode_size != 0)
		return -EIO;
	if (!es->s_inodes_count || !es->s_blocks_per_group ||
	    !es->s_inodes_per_group ||
	    es->s_blocks_count <= es->s_first_data_block)
		return -EIO;
	if (es->s_blocks_per_group > BLOCK_SIZE * 8U ||
	    es->s_inodes_per_group > BLOCK_SIZE * 8U ||
	    es->s_frags_per_group != es->s_blocks_per_group)
		return -EIO;
	if (es->s_r_blocks_count > es->s_blocks_count ||
	    es->s_free_blocks_count >
		    es->s_blocks_count - es->s_first_data_block ||
	    es->s_free_inodes_count > es->s_inodes_count)
		return -EIO;

	device_blocks = blkdev_block_count(dev);
	if (es->s_blocks_count > device_blocks)
		return -EIO;
	if (!ext2_div_round_up_u64((uint64_t)es->s_blocks_count -
					   es->s_first_data_block,
				   es->s_blocks_per_group, &block_groups) ||
	    !ext2_div_round_up_u64(es->s_inodes_count, es->s_inodes_per_group,
				   &inode_groups))
		return -EIO;
	if (!block_groups || inode_groups > block_groups ||
	    block_groups > UINT32_MAX)
		return -EIO;

	if (!ext2_div_round_up_u64(block_groups,
				   BLOCK_SIZE / sizeof(struct ext2_group_desc),
				   &bgdt_blocks))
		return -EIO;
	bgdt_first = (uint64_t)es->s_first_data_block + 1;
	if (!ext2_range_valid(bgdt_first, bgdt_blocks, es->s_blocks_count) ||
	    !ext2_range_valid(bgdt_first, bgdt_blocks, device_blocks))
		return -EIO;
	if (check_mul_overflow(block_groups,
			       (uint64_t)sizeof(struct ext2_group_desc),
			       &bgdt_bytes) ||
	    bgdt_bytes > SIZE_MAX)
		return -EIO;

	geometry->device_blocks = device_blocks;
	geometry->groups_count = (uint32_t)block_groups;
	geometry->inode_size = inode_size;

	return 0;
}

static int ext2_read_bgdt(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	void *buf __cleanup_with(vfree) = NULL;
	uint64_t bytes;
	uint64_t blocks;
	uint64_t first_block;
	uint8_t *dst;

	if (!sbi || !sbi->s_groups_count)
		return -EINVAL;
	if (check_mul_overflow((uint64_t)sbi->s_groups_count,
			       (uint64_t)sizeof(struct ext2_group_desc),
			       &bytes) ||
	    bytes == 0 || bytes > SIZE_MAX ||
	    !ext2_div_round_up_u64(bytes, BLOCK_SIZE, &blocks))
		return -EIO;
	first_block = (uint64_t)sbi->s_first_data_block + 1;
	if (blocks > UINT32_MAX ||
	    !ext2_block_range_valid(sbi, first_block, blocks))
		return -EIO;

	buf = vmalloc((size_t)bytes, ALLOC_NOWAIT);
	if (!buf)
		return -ENOMEM;

	dst = (uint8_t *)buf;
	for (uint64_t block = 0; block < blocks; block++) {
		uint64_t physical = first_block + block;
		uint64_t copied = block * BLOCK_SIZE;
		uint64_t remaining = bytes - copied;
		uint32_t copy = remaining > BLOCK_SIZE ? BLOCK_SIZE
						       : (uint32_t)remaining;
		struct pgcache *page;

		if (!ext2_block_range_valid(sbi, physical, 1))
			return -EIO;
		page = pgcache_get_block(sb->s_dev, physical);
		if (!page)
			return -EIO;

		memcpy(dst + copied, page_cache_data(page), copy);
		pgcache_put_page(page);
	}

	sbi->s_group_desc = buf;
	cleanup_forget_ptr(buf);
	return 0;
}

static int ext2_validate_group_descs(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	uint64_t free_blocks = 0;
	uint64_t free_inodes = 0;

	if (!sbi || !sbi->s_group_desc)
		return -EIO;

	for (uint32_t group = 0; group < sbi->s_groups_count; group++) {
		const struct ext2_group_desc *gd = &sbi->s_group_desc[group];
		uint64_t group_first;
		uint32_t group_blocks;
		uint32_t group_inodes;
		uint64_t table_bytes;
		uint64_t table_blocks;
		int ret;

		ret = ext2_group_geometry(sbi, group, &group_first,
					  &group_blocks, &group_inodes);
		if (ret < 0)
			return ret;
		if (gd->bg_free_blocks_count > group_blocks ||
		    gd->bg_free_inodes_count > group_inodes ||
		    gd->bg_used_dirs_count > group_inodes)
			return -EIO;
		free_blocks += gd->bg_free_blocks_count;
		free_inodes += gd->bg_free_inodes_count;
		if (!ext2_data_block_valid(sbi, gd->bg_block_bitmap) ||
		    !ext2_data_block_valid(sbi, gd->bg_inode_bitmap) ||
		    gd->bg_block_bitmap < group_first ||
		    gd->bg_inode_bitmap < group_first ||
		    gd->bg_block_bitmap >= group_first + group_blocks ||
		    gd->bg_inode_bitmap >= group_first + group_blocks)
			return -EIO;

		if (!group_inodes)
			continue;
		if (check_mul_overflow((uint64_t)group_inodes,
				       (uint64_t)sbi->s_inode_size,
				       &table_bytes) ||
		    !ext2_div_round_up_u64(table_bytes, BLOCK_SIZE,
					   &table_blocks) ||
		    !ext2_block_range_valid(sbi, gd->bg_inode_table,
					    table_blocks) ||
		    gd->bg_inode_table < group_first ||
		    table_blocks > group_blocks ||
		    gd->bg_inode_table - group_first >
			    (uint64_t)group_blocks - table_blocks)
			return -EIO;
	}
	if (free_blocks != sbi->s_es.s_free_blocks_count ||
	    free_inodes != sbi->s_es.s_free_inodes_count)
		return -EIO;

	return 0;
}

static int ext2_read_super(struct super_block *sb)
{
	struct ext2_geometry geometry;
	struct ext2_super_block *es __cleanup_with(kfree) = NULL;
	struct ext2_sb_info *sbi __cleanup_with(kfree) = NULL;
	int ret;

	if (!sb)
		return -EINVAL;

	es = kmalloc(sizeof(*es), ALLOC_NOWAIT);
	if (!es)
		return -ENOMEM;

	ret = ext2_read_super_block(sb->s_dev, es);
	if (ret < 0)
		return ret;
	ret = ext2_check_super_block(sb->s_dev, es, &geometry);
	if (ret < 0)
		return ret;

	sbi = kmalloc(sizeof(*sbi), ALLOC_NOWAIT);
	if (!sbi)
		return -ENOMEM;
	memset(sbi, 0, sizeof(*sbi));
	spin_lock_init(&sbi->s_lock, LOCK_RANK_EXT2_SB, LOCK_IRQ_TASK_ONLY);
	sbi->s_es = *es;
	sbi->s_device_blocks = geometry.device_blocks;
	sbi->s_groups_count = geometry.groups_count;
	sbi->s_inode_size = geometry.inode_size;
	sbi->s_blocks_per_group = sbi->s_es.s_blocks_per_group;
	sbi->s_inodes_per_group = sbi->s_es.s_inodes_per_group;
	sbi->s_first_data_block = sbi->s_es.s_first_data_block;

	sb->s_blocksize = BLOCK_SIZE;
	sb->s_op = &ext2_sops;
	sb->s_private = sbi;
	cleanup_forget_ptr(sbi);

	ret = ext2_read_bgdt(sb);
	if (ret < 0) {
		ext2_free_sbi(sb->s_private);
		sb->s_private = NULL;
		return ret;
	}
	ret = ext2_validate_group_descs(sb);
	if (ret < 0) {
		ext2_free_sbi(sb->s_private);
		sb->s_private = NULL;
		return ret;
	}

	return 0;
}

static int ext2_probe(dev_t dev)
{
	struct ext2_geometry geometry;
	struct ext2_super_block *es __cleanup_with(kfree) = NULL;
	int ret;

	es = kmalloc(sizeof(*es), ALLOC_NOWAIT);
	if (!es)
		return -ENOMEM;

	ret = ext2_read_super_block(dev, es);
	if (ret < 0)
		return ret;
	ret = ext2_check_super_block(dev, es, &geometry);
	if (ret == -ENODEV)
		return 0;
	if (ret < 0)
		return ret;
	return 1;
}

static int ext2_mount(struct file_system_type *fs_type, dev_t dev,
		      const void *data, struct super_block **out_sb)
{
	struct super_block *sb;
	struct inode *root_inode;
	struct dentry *root;
	int ret;

	(void)data;
	if (!out_sb)
		return -EINVAL;
	*out_sb = NULL;

	sb = super_alloc(fs_type, dev);
	if (!sb)
		return -ENOMEM;

	ret = ext2_read_super(sb);
	if (ret < 0) {
		ext2_free_super(sb);
		return ret;
	}

	root = dentry_alloc(NULL, "/", 1);
	if (!root) {
		ext2_free_super(sb);
		return -ENOMEM;
	}
	root->d_sb = sb;
	sb->s_root = root;

	root_inode = iget(sb, EXT2_ROOT_INO);
	if (!root_inode) {
		ext2_free_super(sb);
		return -EIO;
	}

	root->d_inode = root_inode;
	root->d_parent = root;

	*out_sb = sb;
	return 0;
}

int ext2_init(void)
{
	return register_filesystem(&ext2_fs_type);
}

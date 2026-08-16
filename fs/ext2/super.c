#include <nuvix/blkdev.h>
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
static void ext2_evict_inode(struct inode *inode);
static int ext2_statfs(struct super_block *sb, struct statfs64 *buf);

static const struct super_operations ext2_sops = {
	.read_inode = ext2_read_inode,
	.write_inode = ext2_write_inode,
	.datasync_inode = ext2_datasync_inode,
	.evict_inode = ext2_evict_inode,
	.statfs = ext2_statfs,
};

static struct file_system_type ext2_fs_type = {
	.name = "ext2",
	.probe = ext2_probe,
	.mount = ext2_mount,
};

static void ext2_evict_inode(struct inode *inode)
{
	if (!inode)
		return;

	pgcache_invalidate_inode(inode);
	if (inode->i_nlink == 0 && inode->i_private) {
		ext2_free_inode_blocks(inode);
		ext2_free_inode(inode->i_sb, (uint32_t)inode->i_ino);
	}
	kfree(inode->i_private);
	inode->i_private = NULL;
}

static void ext2_free_sbi(struct ext2_sb_info *sbi)
{
	if (!sbi)
		return;

	if (sbi->s_group_desc)
		vfree(sbi->s_group_desc);
	kfree(sbi);
}

static void ext2_free_super(struct super_block *sb)
{
	if (!sb)
		return;

	ext2_free_sbi(EXT2_SB(sb));
	kfree(sb);
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

static uint32_t div_round_up_u32(uint32_t value, uint32_t divisor)
{
	return (value + divisor - 1) / divisor;
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
	page = pgcache_get_block(dev, super_block);
	if (!page)
		return -EIO;

	memcpy(es, page_cache_data(page) + super_off, sizeof(*es));
	pgcache_put_page(page);
	return 0;
}

static int ext2_check_super_block(const struct ext2_super_block *es)
{
	uint32_t inode_size;

	if (!es)
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
	if (es->s_log_block_size != 2)
		return -EINVAL;

	inode_size = es->s_rev_level == EXT2_GOOD_OLD_REV
			     ? EXT2_GOOD_OLD_INODE_SIZE
			     : es->s_inode_size;
	if (inode_size < sizeof(struct ext2_inode) || inode_size > BLOCK_SIZE)
		return -EINVAL;
	if (!es->s_blocks_per_group || !es->s_inodes_per_group ||
	    es->s_blocks_count <= es->s_first_data_block)
		return -EINVAL;

	return 0;
}

static int ext2_read_bgdt(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	uint32_t desc_per_block = BLOCK_SIZE / sizeof(struct ext2_group_desc);
	uint32_t bytes = sbi->s_groups_count * sizeof(struct ext2_group_desc);
	uint32_t first_block = EXT2_BGDT_BLOCK(sbi->s_first_data_block);
	uint8_t *dst;

	sbi->s_group_desc = vmalloc(bytes, ALLOC_NOWAIT);
	if (!sbi->s_group_desc)
		return -ENOMEM;

	dst = (uint8_t *)sbi->s_group_desc;
	for (uint32_t block = 0;
	     block < div_round_up_u32(sbi->s_groups_count, desc_per_block);
	     block++) {
		struct pgcache *page =
			pgcache_get_block(sb->s_dev, first_block + block);
		uint32_t copy = bytes - block * BLOCK_SIZE;

		if (!page) {
			vfree(sbi->s_group_desc);
			sbi->s_group_desc = NULL;
			return -EIO;
		}
		if (copy > BLOCK_SIZE)
			copy = BLOCK_SIZE;

		memcpy(dst + block * BLOCK_SIZE, page_cache_data(page), copy);
		pgcache_put_page(page);
	}

	return 0;
}

static int ext2_read_super(struct super_block *sb)
{
	struct ext2_sb_info *sbi;
	struct ext2_super_block *es;
	int ret;

	if (!sb)
		return -EINVAL;

	es = kmalloc(sizeof(*es), ALLOC_NOWAIT);
	if (!es)
		return -ENOMEM;

	ret = ext2_read_super_block(sb->s_dev, es);
	if (ret < 0)
		return ret;
	ret = ext2_check_super_block(es);
	if (ret == -ENODEV)
		return -EINVAL;
	if (ret < 0)
		return ret;

	sbi = kmalloc(sizeof(*sbi), ALLOC_NOWAIT);
	if (!sbi)
		return -ENOMEM;
	memset(sbi, 0, sizeof(*sbi));
	spin_lock_init(&sbi->s_lock, LOCK_RANK_EXT2_SB, LOCK_IRQ_TASK_ONLY);
	sbi->s_es = *es;

	sbi->s_inode_size = sbi->s_es.s_rev_level == EXT2_GOOD_OLD_REV
				    ? EXT2_GOOD_OLD_INODE_SIZE
				    : sbi->s_es.s_inode_size;

	sbi->s_blocks_per_group = sbi->s_es.s_blocks_per_group;
	sbi->s_inodes_per_group = sbi->s_es.s_inodes_per_group;
	sbi->s_first_data_block = sbi->s_es.s_first_data_block;

	sbi->s_groups_count = div_round_up_u32(sbi->s_es.s_blocks_count -
						       sbi->s_first_data_block,
					       sbi->s_blocks_per_group);

	sb->s_blocksize = BLOCK_SIZE;
	sb->s_op = &ext2_sops;
	sb->s_private = sbi;

	ret = ext2_read_bgdt(sb);
	if (ret < 0) {
		ext2_free_sbi(sbi);
		sb->s_private = NULL;
		return ret;
	}

	return 0;
}

static int ext2_probe(dev_t dev)
{
	struct ext2_super_block *es;
	int ret;

	es = kmalloc(sizeof(*es), ALLOC_NOWAIT);
	if (!es)
		return -ENOMEM;

	ret = ext2_read_super_block(dev, es);
	if (ret < 0)
		return ret;
	ret = ext2_check_super_block(es);
	if (ret == -ENODEV)
		return 0;
	if (ret < 0)
		return ret;
	kfree(es);
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

	root_inode = iget(sb, EXT2_ROOT_INO);
	if (!root_inode) {
		kfree(root);
		ext2_free_super(sb);
		return -EIO;
	}

	root->d_inode = root_inode;
	root->d_sb = sb;
	root->d_parent = root;
	sb->s_root = root;

	*out_sb = sb;
	return 0;
}

int ext2_init(void)
{
	return register_filesystem(&ext2_fs_type);
}

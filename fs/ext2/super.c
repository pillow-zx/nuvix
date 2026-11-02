#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/layout.h>
#include <nuvix/macro.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/vfs.h>

#include "ext2.h"

static struct ext2_fs ext2_data;

static int ext2_read_metadata_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct ext2_fs *fs = m->m_host;
    uint64_t sector;

    if (cp->index >= fs->blocks_count)
        return -EIO;

    sector = cp->index * PAGE_SIZE / BLK_SECTOR_SIZE;
    return blk_read(m->m_bdev, sector, cp->page, 0, PAGE_SIZE);
}

static const struct mapping_ops ext2_metadata_ops = {
    .read_page = ext2_read_metadata_page,
};

int ext2_read_block(struct super_block *sb, uint32_t block, void *buffer)
{
    struct ext2_fs *fs = ext2_sb_info(sb);
    struct page *page __free(page) = NULL;
    uint64_t sector;
    int ret;

    if (!sb->s_bdev)
        return -ENODEV;
    if (!fs->block_size || fs->block_size > PAGE_SIZE)
        return -EOPNOTSUPP;
    if (block >= fs->blocks_count)
        return -EIO;

    page = alloc_page();
    if (!page)
        return -ENOMEM;

    sector = (uint64_t)block * fs->block_size / BLK_SECTOR_SIZE;
    ret = blk_read(sb->s_bdev, sector, page, 0, fs->block_size);
    if (ret)
        return ret;

    memcpy(buffer, (void *)__va(page_to_phys(page)), fs->block_size);
    return 0;
}

int ext2_mount(struct super_block *sb)
{
    struct page *page __free(page) = NULL;
    struct ext2_super_block *raw;
    struct ext2_fs *fs = &ext2_data;
    struct inode *root_inode;
    struct dentry *root;
    uint32_t block_size;
    uint32_t inode_size;
    int ret;

    if (!sb->s_bdev)
        return -ENODEV;

    page = alloc_page();
    if (!page)
        return -ENOMEM;

    ret = blk_read(sb->s_bdev, 2, page, 0, BLK_SECTOR_SIZE);
    if (ret)
        return ret;

    raw = (struct ext2_super_block *)__va(page_to_phys(page));
    if (raw->s_magic != EXT2_SUPER_MAGIC)
        return -EINVAL;
    if (raw->s_log_block_size != 2)
        return -EOPNOTSUPP;

    block_size = 1024U << raw->s_log_block_size;
    inode_size =
        raw->s_rev_level == 0 ? EXT2_GOOD_OLD_INODE_SIZE : raw->s_inode_size;
    if (inode_size < EXT2_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        block_size % inode_size)
        return -EINVAL;
    if (!raw->s_blocks_count || !raw->s_inodes_count ||
        !raw->s_blocks_per_group || !raw->s_inodes_per_group)
        return -EINVAL;

    fs->block_size = block_size;
    fs->blocks_count = raw->s_blocks_count;
    fs->inodes_count = raw->s_inodes_count;
    fs->first_data_block = raw->s_first_data_block;
    fs->blocks_per_group = raw->s_blocks_per_group;
    fs->inodes_per_group = raw->s_inodes_per_group;
    fs->inode_size = inode_size;
    fs->first_ino = raw->s_rev_level == 0 ? 11 : raw->s_first_ino;
    fs->features_incompat = raw->s_feature_incompat;
    fs->block_mapping = (struct mapping){
        .m_bdev = sb->s_bdev,
        .m_ops = &ext2_metadata_ops,
        .m_host = fs,
    };
    sb->s_fs_info = fs;

    root_inode = ext2_iget(sb, EXT2_ROOT_INO);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto fail;
    }
    if (!S_ISDIR(root_inode->i_mode)) {
        iput(root_inode);
        ret = -EINVAL;
        goto fail;
    }

    root = d_alloc(NULL, "/", 1);
    if (IS_ERR(root)) {
        ret = PTR_ERR(root);
        iput(root_inode);
        goto fail;
    }

    root->d_inode = root_inode;
    sb->s_root = root;
    return 0;

fail:
    mapping_purge(&fs->block_mapping, 0, fs->blocks_count);
    sb->s_fs_info = NULL;
    return ret;
}

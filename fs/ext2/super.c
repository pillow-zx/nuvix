#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/page.h>
#include <nuvix/vfs.h>

#include "ext2.h"

static struct ext2_sb_info ext2_data;

static int ext2_read_metadata_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct ext2_sb_info *sbi = m->m_host;
    uint64_t sector;

    if (cp->index && cp->index >= sbi->s_es->s_blocks_count)
        return -EIO;

    sector = cp->index * PAGE_SIZE / BLK_SECTOR_SIZE;
    return blk_read(m->m_bdev, sector, cp->page, 0, PAGE_SIZE);
}

static const struct mapping_ops ext2_metadata_mapping_ops = {
    .read_page = ext2_read_metadata_page,
};

int ext2_mount(struct super_block *sb)
{
    struct ext2_super_block *es;
    struct ext2_sb_info *sbi = &ext2_data;
    struct inode *root_inode;
    struct dentry *root;
    uint32_t block_size;
    uint32_t inode_size;
    int ret;

    if (!sb->s_bdev)
        return -ENODEV;

    sbi->mapping = MAPPING_INIT(sb->s_bdev, ext2_metadata_mapping_ops, sbi);
    sbi->s_sbh = mapping_get(&sbi->mapping, 0);
    if (IS_ERR(sbi->s_sbh)) {
        ret = PTR_ERR(sbi->s_sbh);
        sbi->s_sbh = NULL;
        return ret;
    }
    mutex_lock(&sbi->s_sbh->mutex);
    es = (struct ext2_super_block *)((char *)mapping_data(sbi->s_sbh) +
                                     EXT2_SUPER_OFFSET);
    sbi->s_es = es;
    mutex_unlock(&sbi->s_sbh->mutex);
    if (es->s_magic != EXT2_SUPER_MAGIC) {
        ret = -EINVAL;
        goto fail;
    }
    if (es->s_log_block_size != 2) {
        ret = -EOPNOTSUPP;
        goto fail;
    }

    block_size = 1024U << es->s_log_block_size;
    inode_size =
        es->s_rev_level == 0 ? EXT2_GOOD_OLD_INODE_SIZE : es->s_inode_size;
    if (inode_size < EXT2_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        block_size % inode_size) {
        ret = -EINVAL;
        goto fail;
    }
    if (!es->s_blocks_count || !es->s_inodes_count || !es->s_blocks_per_group ||
        !es->s_inodes_per_group) {
        ret = -EINVAL;
        goto fail;
    }

    sbi->block_size = block_size;
    sbi->inode_size = inode_size;
    sb->s_fs_info = sbi;

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
    mapping_purge(&sbi->mapping, 0, UINT64_MAX);
    mapping_put(sbi->s_sbh);
    sbi->s_sbh = NULL;
    sbi->s_es = NULL;
    sb->s_fs_info = NULL;
    return ret;
}

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
    uint64_t sect;

    if (cp->index && cp->index >= sbi->s_es->s_blocks_count)
        return -EIO;

    sect = cp->index * PAGE_SIZE / BLK_SECTOR_SIZE;
    return blk_read(m->m_bdev, sect, cp->page, 0, PAGE_SIZE);
}

static int ext2_write_metadata_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    uint64_t sect = cp->index * PAGE_SIZE / BLK_SECTOR_SIZE;

    return blk_write(m->m_bdev, sect, cp->page, 0, PAGE_SIZE);
}

static const struct mapping_ops ext2_metadata_mapping_ops = {
    .read_page = ext2_read_metadata_page,
    .write_page = ext2_write_metadata_page,
};

static int ext2_check_super(struct ext2_sb_info *sbi)
{
    struct ext2_super_block *es = sbi->s_es;
    uint32_t bsize;
    uint32_t isize;

    if (es->s_magic != EXT2_SUPER_MAGIC)
        return -EINVAL;
    if (es->s_log_block_size != 2)
        return -EOPNOTSUPP;

    bsize = 1024U << es->s_log_block_size;
    isize = es->s_rev_level == 0 ? EXT2_GOOD_OLD_INODE_SIZE : es->s_inode_size;
    if (isize < EXT2_GOOD_OLD_INODE_SIZE || isize > bsize || bsize % isize)
        return -EINVAL;
    if (!es->s_blocks_count || !es->s_inodes_count || !es->s_blocks_per_group ||
        !es->s_inodes_per_group)
        return -EINVAL;

    sbi->block_size = bsize;
    sbi->inode_size = isize;
    return 0;
}

static struct dentry *ext2_get_root(struct super_block *sb)
{
    struct inode *inode;
    struct dentry *root;

    inode = ext2_iget(sb, EXT2_ROOT_INO);
    if (IS_ERR(inode))
        return ERR_PTR(PTR_ERR(inode));
    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        return ERR_PTR(-EINVAL);
    }

    root = d_alloc(NULL, "/", 1);
    if (IS_ERR(root)) {
        iput(inode);
        return root;
    }

    root->d_inode = inode;
    return root;
}

int ext2_mount(struct super_block *sb)
{
    struct ext2_super_block *es;
    struct ext2_sb_info *sbi = &ext2_data;
    struct dentry *root;
    int ret;

    if (!sb->s_bdev)
        return -ENODEV;

    mutex_init(&sbi->mutex);
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
    ret = ext2_check_super(sbi);
    if (ret)
        goto fail;
    sb->s_fs_info = sbi;

    root = ext2_get_root(sb);
    if (IS_ERR(root)) {
        ret = PTR_ERR(root);
        goto fail;
    }
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

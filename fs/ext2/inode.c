#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/mutex.h>
#include <nuvix/mapping.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/macro.h>
#include <nuvix/printk.h>

#include "ext2.h"

static int ext2_inode_location(struct ext2_sb_info *sbi, uint32_t ino,
                               uint32_t *blk, uint32_t *off)
{
    struct cache_page *cp __free(cache_page) = NULL;
    struct ext2_group_desc *desc;
    uint32_t grp = (ino - 1) / sbi->s_es->s_inodes_per_group;
    uint32_t idx = (ino - 1) % sbi->s_es->s_inodes_per_group;
    uint64_t pos = (uint64_t)idx * sbi->inode_size;
    uint32_t doff;

    cp = ext2_get_grp_desc(sbi, grp, &doff);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    mutex_lock(&cp->mutex);
    desc = (struct ext2_group_desc *)((char *)mapping_data(cp) + doff);
    *blk = desc->bg_inode_table + pos / sbi->block_size;
    mutex_unlock(&cp->mutex);
    *off = pos % sbi->block_size;
    return 0;
}

static int ext2_read_inode(struct super_block *sb, uint32_t ino,
                           struct ext2_inode *raw)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct cache_page *cp __free(cache_page) = NULL;
    uint32_t blk;
    uint32_t off;
    int ret;

    ret = ext2_inode_location(sbi, ino, &blk, &off);
    if (ret)
        return ret;
    if (off + sizeof(*raw) > sbi->block_size)
        return -EINVAL;
    cp = mapping_get(&sbi->mapping, blk);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    mutex_lock(&cp->mutex);
    memcpy(raw, (const char *)mapping_data(cp) + off, sizeof(*raw));
    mutex_unlock(&cp->mutex);
    if (!raw->i_mode)
        return -ENOENT;
    return 0;
}

struct cache_page *ext2_get_inode_page(struct inode *inode, uint32_t *off)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    uint32_t blk;
    int ret;

    assert_mutex_locked(&inode->mutex);
    ret = ext2_inode_location(sbi, inode->i_ino, &blk, off);
    if (ret)
        return ERR_PTR(ret);
    return mapping_get(&sbi->mapping, blk);
}

void ext2_write_inode(struct inode *inode, struct cache_page *cp, uint32_t off)
{
    struct ext2_inode *raw;

    assert_mutex_locked(&inode->mutex);
    mutex_lock(&cp->mutex);
    raw = (struct ext2_inode *)((char *)mapping_data(cp) + off);
    raw->i_mode = inode->i_mode;
    raw->i_links_count = inode->i_nlink;
    raw->i_size = inode->i_size;
    raw->i_blocks = inode->i_blocks;
    memcpy(raw->i_block, ext2_i(inode)->data, sizeof(raw->i_block));
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
}

static int ext2_getattr(struct inode *inode, struct kstat *stat)
{
    mutex_lock(&inode->mutex);
    stat->dev = 0;
    stat->ino = inode->i_ino;
    stat->rdev = 0;
    stat->mode = (inode->i_mode & S_IFMT) | 0777;
    stat->nlink = inode->i_nlink;
    stat->uid = 0;
    stat->gid = 0;
    stat->size = inode->i_size;
    stat->blocks = inode->i_blocks;
    stat->blksize = ext2_sb_info(inode->i_sb)->block_size;
    stat->atime = inode->i_atime;
    stat->mtime = inode->i_mtime;
    stat->ctime = inode->i_ctime;
    mutex_unlock(&inode->mutex);
    return 0;
}

static int ext2_zero_tail(struct inode *inode, loff_t pos)
{
    struct cache_page *cp __free(cache_page) = NULL;
    uint64_t index = (uint64_t)pos / PAGE_SIZE;
    size_t off = (uint64_t)pos % PAGE_SIZE;
    uint32_t blk;
    int ret;

    assert_mutex_locked(&inode->mutex);

    if (!off)
        return 0;

    ret = ext2_bmap(inode, index, &blk);
    if (ret)
        return ret;
    if (!blk)
        return 0;

    cp = mapping_get(&ext2_i(inode)->mapping, index);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    mutex_lock(&cp->mutex);
    memset((char *)mapping_data(cp) + off, 0, PAGE_SIZE - off);
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);

    return 0;
}

static int ext2_setsize(struct inode *inode, loff_t nsize)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct cache_page *cp __free(cache_page) = NULL;
    loff_t oldsize = inode->i_size;
    loff_t tail = MIN(oldsize, nsize);
    uint32_t off;
    int ret;

    assert_mutex_locked(&inode->mutex);
    Assert(S_ISREG(inode->i_mode) && nsize >= 0,
           "resize requires a regular file and a nonnegative size\n");
    if ((uint64_t)nsize > UINT32_MAX)
        return -EFBIG;

    cp = ext2_get_inode_page(inode, &off);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    ret = ext2_zero_tail(inode, tail);
    if (ret)
        return ret;

    if (nsize < oldsize) {
        uint64_t first = DIV_ROUND_UP((uint64_t)nsize, PAGE_SIZE);

        mapping_purge(&ext2_i(inode)->mapping, first, UINT64_MAX);
        ret = ext2_truncate_blocks(
            inode, DIV_ROUND_UP((uint64_t)nsize, sbi->block_size));
        if (ret) {
            ext2_write_inode(inode, cp, off);
            return ret;
        }
    }

    inode->i_size = nsize;
    ext2_write_inode(inode, cp, off);
    return 0;
}

static int ext2_setattr(struct inode *inode, const struct iattr *attr)
{
    int ret = 0;

    mutex_lock(&inode->mutex);
    if (attr->ia_valid & ATTR_SIZE)
        ret = ext2_setsize(inode, attr->ia_size);
    mutex_unlock(&inode->mutex);

    return ret;
}

static const struct inode_operations ext2_inode_ops = {
    .lookup = ext2_lookup,
    .getattr = ext2_getattr,
    .setattr = ext2_setattr,
};

static struct inode *ext2_alloc_inode_info(struct super_block *sb)
{
    struct ext2_inode_info *info;
    struct inode *inode;

    info = kzalloc(sizeof(*info));
    if (!info)
        return NULL;
    inode = &info->vfs_inode;
    inode->i_ops = &ext2_inode_ops;
    info->mapping = MAPPING_INIT(sb->s_bdev, ext2_file_mapping_operations, inode);
    info->mapping.mutex = &inode->mutex;
    return inode;
}

static void ext2_evict_inode(struct inode *inode)
{
    struct ext2_inode_info *info = ext2_i(inode);
    struct cache_page *cp __free(cache_page) = NULL;
    uint32_t off;
    int ret;

    mutex_lock(&inode->mutex);
    mapping_purge(&info->mapping, 0, UINT64_MAX);
    if (inode->i_nlink)
        goto out;

    cp = ext2_get_inode_page(inode, &off);
    if (IS_ERR(cp)) {
        ret = PTR_ERR(move(cp));
        pr_err("ext2: inode %u record read failed during eviction (%d)\n",
               (uint32_t)inode->i_ino, ret);
    }

    if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
        (S_ISLNK(inode->i_mode) && inode->i_blocks)) {
        ext2_free_blocks(inode);
    }
    inode->i_size = 0;
    inode->i_blocks = 0;
    if (cp) {
        struct ext2_inode *raw;

        mutex_lock(&cp->mutex);
        raw = (struct ext2_inode *)((char *)mapping_data(cp) + off);
        raw->i_mode = 0;
        raw->i_links_count = 0;
        raw->i_size = 0;
        raw->i_blocks = 0;
        memset(raw->i_block, 0, sizeof(raw->i_block));
        mapping_dirty(cp);
        mutex_unlock(&cp->mutex);
    }
    ext2_free_inode(inode);
out:
    mutex_unlock(&inode->mutex);
}

static void ext2_destroy_inode(struct inode *inode)
{
    kfree(ext2_i(inode));
}

const struct super_operations ext2_super_operations = {
    .alloc_inode = ext2_alloc_inode_info,
    .evict_inode = ext2_evict_inode,
    .destroy_inode = ext2_destroy_inode,
};

struct inode *ext2_iget(struct super_block *sb, uint32_t ino)
{
    struct ext2_inode_info *info;
    struct ext2_inode raw;
    struct inode *inode;
    int ret;

    if (!ino || ino > ext2_sb_info(sb)->s_es->s_inodes_count)
        return ERR_PTR(-EINVAL);
    inode = icache_find(sb, ino);
    if (inode)
        return inode;
    inode = i_alloc(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    inode->i_ino = ino;
    inode = icache_insert(inode);
    if (!(inode->i_state & I_NEW))
        return inode;
    ret = ext2_read_inode(sb, ino, &raw);
    if (ret) {
        icache_abort(inode);
        iput(inode);
        return ERR_PTR(ret);
    }

    info = ext2_i(inode);
    memcpy(info->data, raw.i_block, sizeof(info->data));
    inode->i_size = raw.i_size;
    inode->i_blocks = raw.i_blocks;
    inode->i_atime.tv_sec = raw.i_atime;
    inode->i_mtime.tv_sec = raw.i_mtime;
    inode->i_ctime.tv_sec = raw.i_ctime;
    inode->i_mode = raw.i_mode;
    inode->i_nlink = raw.i_links_count;
    if (S_ISDIR(inode->i_mode))
        inode->i_fops = &ext2_dir_operations;
    else if (S_ISREG(inode->i_mode))
        inode->i_fops = &ext2_file_operations;
    icache_ready(inode);
    return inode;
}

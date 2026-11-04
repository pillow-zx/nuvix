#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/macro.h>

#include "ext2.h"

static int ext2_read_ptr(struct super_block *sb, uint32_t block, uint32_t index,
                         uint32_t *value)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct cache_page *cp __free(cache_page) = NULL;
    const uint32_t *ptr;

    if (!block) {
        *value = 0;
        return 0;
    }

    cp = mapping_get(&sbi->mapping, block);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    mutex_lock(&cp->mutex);
    ptr = mapping_data(cp);
    *value = ptr[index];
    mutex_unlock(&cp->mutex);
    return 0;
}

static int ext2_read_raw_inode(struct super_block *sb, uint32_t ino,
                               struct ext2_inode *raw)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct ext2_group_desc desc;
    struct cache_page *cp __free(cache_page) = NULL;
    uint32_t desc_block;
    uint32_t desc_per_block;
    uint32_t group;
    uint32_t index;
    uint32_t inode_block;
    uint32_t inode_offset;
    uint32_t offset_in_block;

    group = (ino - 1) / sbi->s_es->s_inodes_per_group;
    index = (ino - 1) % sbi->s_es->s_inodes_per_group;
    desc_per_block = sbi->block_size / sizeof(struct ext2_group_desc);
    desc_block = sbi->s_es->s_first_data_block + 1 + group / desc_per_block;

    cp = mapping_get(&sbi->mapping, desc_block);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    mutex_lock(&cp->mutex);
    memcpy(&desc,
           (const char *)mapping_data(cp) +
               (group % desc_per_block) * sizeof(struct ext2_group_desc),
           sizeof(desc));
    mutex_unlock(&cp->mutex);
    mapping_put(move(cp));

    inode_offset = index * sbi->inode_size;
    inode_block = desc.bg_inode_table + inode_offset / sbi->block_size;
    offset_in_block = inode_offset % sbi->block_size;
    if (offset_in_block + sizeof(*raw) > sbi->block_size)
        return -EINVAL;

    cp = mapping_get(&sbi->mapping, inode_block);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    mutex_lock(&cp->mutex);
    memcpy(raw, (const char *)mapping_data(cp) + offset_in_block, sizeof(*raw));
    mutex_unlock(&cp->mutex);
    if (!raw->i_mode)
        return -ENOENT;
    return 0;
}

int ext2_bmap(struct inode *inode, uint32_t logical, uint32_t *block)
{
    struct super_block *sb = inode->i_sb;
    struct ext2_inode_info *info = ext2_i(inode);
    uint32_t ptrs = ext2_sb_info(sb)->block_size / sizeof(uint32_t);
    uint64_t l = logical;
    uint32_t first;
    uint32_t second;
    int ret;

    assert_mutex_locked(&inode->mutex);
    if (l < EXT2_NDIR_BLOCKS) {
        *block = info->data[l];
        return 0;
    }
    l -= EXT2_NDIR_BLOCKS;

    if (l < ptrs)
        return ext2_read_ptr(sb, info->data[EXT2_IND_BLOCK], (uint32_t)l,
                             block);

    l -= ptrs;
    if (l < (uint64_t)ptrs * ptrs) {
        ret = ext2_read_ptr(sb, info->data[EXT2_DIND_BLOCK],
                            (uint32_t)(l / ptrs), &first);
        if (ret)
            return ret;
        return ext2_read_ptr(sb, first, (uint32_t)(l % ptrs), block);
    }

    l -= (uint64_t)ptrs * ptrs;
    if (l < (uint64_t)ptrs * ptrs * ptrs) {
        ret = ext2_read_ptr(sb, info->data[EXT2_TIND_BLOCK],
                            (uint32_t)(l / ((uint64_t)ptrs * ptrs)), &first);
        if (ret)
            return ret;
        ret = ext2_read_ptr(sb, first, (uint32_t)((l / ptrs) % ptrs), &second);
        if (ret)
            return ret;
        return ext2_read_ptr(sb, second, (uint32_t)(l % ptrs), block);
    }

    return -EFBIG;
}

static int ext2_read_file_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct inode *inode = m->m_host;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    uint64_t offset = cp->index * PAGE_SIZE;
    size_t length = MIN((uint64_t)inode->i_size - offset, (uint64_t)PAGE_SIZE);
    char *data = mapping_data(cp);
    uint32_t block;
    uint64_t sector;
    int ret;

    ret = ext2_bmap(inode, (uint32_t)cp->index, &block);
    if (ret)
        return ret;
    if (!block) {
        memset(data, 0, PAGE_SIZE);
        return 0;
    }
    if (block >= sbi->s_es->s_blocks_count)
        return -EIO;

    sector = (uint64_t)block * PAGE_SIZE / BLK_SECTOR_SIZE;
    ret = blk_read(m->m_bdev, sector, cp->page, 0, PAGE_SIZE);
    if (ret)
        return ret;

    memset(data + length, 0, PAGE_SIZE - length);
    return 0;
}

static void ext2_mapping_get(struct mapping *m)
{
    ihold(m->m_host);
}

static void ext2_mapping_put(struct mapping *m)
{
    iput(m->m_host);
}

static const struct mapping_ops ext2_file_mapping_ops = {
    .get = ext2_mapping_get,
    .put = ext2_mapping_put,
    .read_page = ext2_read_file_page,
};

static void ext2_destroy(struct inode *inode)
{
    struct ext2_inode_info *info = ext2_i(inode);

    mutex_lock(&inode->mutex);
    mapping_purge(&info->mapping, 0, UINT64_MAX);
    mutex_unlock(&inode->mutex);
    kfree(info);
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

static const struct inode_operations ext2_inode_ops = {
    .lookup = ext2_lookup,
    .getattr = ext2_getattr,
    .destroy = ext2_destroy,
};

struct inode *ext2_iget(struct super_block *sb, uint32_t ino)
{
    struct ext2_inode_info *info __free(kfree) = NULL;
    struct ext2_inode raw;
    struct inode *inode;
    int ret;

    if (!ino || ino > ext2_sb_info(sb)->s_es->s_inodes_count)
        return ERR_PTR(-EINVAL);

    inode = icache_find(sb, ino);
    if (inode)
        return inode;

    ret = ext2_read_raw_inode(sb, ino, &raw);
    if (ret)
        return ERR_PTR(ret);

    info = kzalloc(sizeof(*info));
    if (!info)
        return ERR_PTR(-ENOMEM);
    memcpy(info->data, raw.i_block, sizeof(info->data));

    inode = &info->vfs_inode;
    refcount_set(&inode->i_count, 1);
    mutex_init(&inode->mutex);
    inode->i_sb = sb;
    inode->i_ino = ino;
    inode->i_size = raw.i_size;
    inode->i_blocks = raw.i_blocks;
    inode->i_atime.tv_sec = raw.i_atime;
    inode->i_mtime.tv_sec = raw.i_mtime;
    inode->i_ctime.tv_sec = raw.i_ctime;
    inode->i_mode = raw.i_mode;
    inode->i_nlink = raw.i_links_count;
    inode->i_ops = &ext2_inode_ops;
    if (S_ISDIR(inode->i_mode))
        inode->i_fops = &ext2_dir_ops;
    else if (S_ISREG(inode->i_mode))
        inode->i_fops = &ext2_file_ops;
    info->mapping = MAPPING_INIT(sb->s_bdev, ext2_file_mapping_ops, inode);
    info->mapping.mutex = &inode->mutex;
    keep(info);

    return icache_insert(inode);
}

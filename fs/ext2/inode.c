#include <nuvix/blkdev.h>
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

#include "ext2.h"

static int ext2_read_ptr(struct super_block *sb, uint32_t blk, uint32_t idx,
                         uint32_t *val)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct cache_page *cp __free(cache_page) = NULL;
    const uint32_t *ptr;

    if (!blk) {
        *val = 0;
        return 0;
    }

    cp = mapping_get(&sbi->mapping, blk);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    mutex_lock(&cp->mutex);
    ptr = mapping_data(cp);
    *val = ptr[idx];
    mutex_unlock(&cp->mutex);
    return 0;
}

static int ext2_inode_location(struct ext2_sb_info *sbi, uint32_t ino,
                               uint32_t *blk, uint32_t *off)
{
    struct cache_page *cp __free(cache_page) = NULL;
    struct ext2_group_desc *desc;
    uint32_t grp = (ino - 1) / sbi->s_es->s_inodes_per_group;
    uint32_t idx = (ino - 1) % sbi->s_es->s_inodes_per_group;
    uint64_t pos = (uint64_t)idx * sbi->inode_size;
    uint32_t doff;

    cp = ext2_get_group_desc(sbi, grp, &doff);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    mutex_lock(&cp->mutex);
    desc = (struct ext2_group_desc *)((char *)mapping_data(cp) + doff);
    *blk = desc->bg_inode_table + pos / sbi->block_size;
    mutex_unlock(&cp->mutex);
    *off = pos % sbi->block_size;
    return 0;
}

static int ext2_read_raw_inode(struct super_block *sb, uint32_t ino,
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
    raw->i_size = inode->i_size;
    raw->i_blocks = inode->i_blocks;
    memcpy(raw->i_block, ext2_i(inode)->data, sizeof(raw->i_block));
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
}

static int ext2_block_to_path(struct ext2_sb_info *sbi, uint32_t lbn,
                              uint32_t offs[4])
{
    uint32_t ptrs = sbi->block_size / sizeof(uint32_t);
    uint64_t n = lbn;

    if (n < EXT2_NDIR_BLOCKS) {
        offs[0] = n;
        return 1;
    }
    n -= EXT2_NDIR_BLOCKS;
    if (n < ptrs) {
        offs[0] = EXT2_IND_BLOCK;
        offs[1] = n;
        return 2;
    }
    n -= ptrs;
    if (n < (uint64_t)ptrs * ptrs) {
        offs[0] = EXT2_DIND_BLOCK;
        offs[1] = n / ptrs;
        offs[2] = n % ptrs;
        return 3;
    }
    n -= (uint64_t)ptrs * ptrs;
    if (n >= (uint64_t)ptrs * ptrs * ptrs)
        return -EFBIG;
    offs[0] = EXT2_TIND_BLOCK;
    offs[1] = n / ((uint64_t)ptrs * ptrs);
    offs[2] = (n / ptrs) % ptrs;
    offs[3] = n % ptrs;
    return 4;
}

int ext2_bmap(struct inode *inode, uint32_t lbn, uint32_t *blk)
{
    uint32_t offs[4];
    uint32_t phys;
    int depth;
    int ret;

    assert_mutex_locked(&inode->mutex);
    depth = ext2_block_to_path(ext2_sb_info(inode->i_sb), lbn, offs);
    if (depth < 0)
        return depth;
    phys = ext2_i(inode)->data[offs[0]];
    for (int i = 1; i < depth; i++) {
        ret = ext2_read_ptr(inode->i_sb, phys, offs[i], &phys);
        if (ret)
            return ret;
    }
    *blk = phys;
    return 0;
}

/* Returns the level of the data block or the first missing pointer. */
static int ext2_get_branch(struct inode *inode, unsigned int depth,
                           const uint32_t *offs, struct cache_page **chain,
                           uint32_t *phys)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    unsigned int level = 0;

    *phys = ext2_i(inode)->data[offs[0]];
    while (*phys && level < depth - 1) {
        struct cache_page *cp;

        cp = mapping_get(&sbi->mapping, *phys);
        if (IS_ERR(cp))
            return PTR_ERR(cp);
        chain[level] = cp;
        mutex_lock(&cp->mutex);
        *phys = ((const uint32_t *)mapping_data(cp))[offs[level + 1]];
        mutex_unlock(&cp->mutex);
        level++;
    }
    return level;
}

static void ext2_put_branch(struct cache_page **pages, unsigned int nr)
{
    for (unsigned int i = 0; i < nr; i++) {
        if (pages[i])
            mapping_put(pages[i]);
    }
}

static int ext2_free_branch(struct super_block *sb, const uint32_t *blks,
                            unsigned int nr)
{
    int ret = 0;

    while (nr) {
        uint32_t phys = blks[--nr];
        int err;

        err = ext2_free_block(sb, phys);
        if (err && !ret)
            ret = err;
    }
    return ret;
}

static int ext2_alloc_branch(struct super_block *sb, unsigned int need,
                             const uint32_t *offs, uint32_t *blks,
                             struct cache_page **fresh, unsigned int *nr)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    int ret;

    for (; *nr < need; (*nr)++) {
        ret = ext2_alloc_block(sb, &blks[*nr]);
        if (ret)
            return ret;
        mapping_purge(&sbi->mapping, blks[*nr], (uint64_t)blks[*nr] + 1);
    }
    /* Acquire every indirect page before modifying the new branch. */
    for (unsigned int i = 0; i < need - 1; i++) {
        struct cache_page *cp = mapping_get(&sbi->mapping, blks[i]);

        if (IS_ERR(cp))
            return PTR_ERR(cp);
        fresh[i] = cp;
    }
    for (unsigned int i = 0; i < need - 1; i++) {
        mutex_lock(&fresh[i]->mutex);
        memset(mapping_data(fresh[i]), 0, PAGE_SIZE);
        ((uint32_t *)mapping_data(fresh[i]))[offs[i]] = blks[i + 1];
        mapping_dirty(fresh[i]);
        mutex_unlock(&fresh[i]->mutex);
    }
    return 0;
}

static void ext2_splice_branch(struct inode *inode, struct cache_page *parent,
                               uint32_t off, uint32_t blk, unsigned int need)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);

    if (!parent) {
        ext2_i(inode)->data[off] = blk;
    } else {
        mutex_lock(&parent->mutex);
        ((uint32_t *)mapping_data(parent))[off] = blk;
        mapping_dirty(parent);
        mutex_unlock(&parent->mutex);
    }
    inode->i_blocks += need * (sbi->block_size / BLK_SECTOR_SIZE);
}

int ext2_bmap_create(struct inode *inode, uint32_t lbn, uint32_t *blk)
{
    struct cache_page *chain[3] = {0};
    struct cache_page *fresh[3] = {0};
    uint32_t blks[4];
    uint32_t offs[4];
    uint32_t phys;
    unsigned int nr = 0;
    unsigned int need;
    int depth;
    int level;
    int ret;
    int err = 0;

    assert_mutex_locked(&inode->mutex);
    depth = ext2_block_to_path(ext2_sb_info(inode->i_sb), lbn, offs);
    if (depth < 0)
        return depth;
    level = ext2_get_branch(inode, depth, offs, chain, &phys);
    if (level < 0) {
        ret = level;
        goto out;
    }
    if (phys) {
        *blk = phys;
        ret = 0;
        goto out;
    }
    need = depth - level;
    ret = ext2_alloc_branch(inode->i_sb, need, offs + level + 1, blks, fresh,
                            &nr);
    if (ret)
        goto out;
    ext2_splice_branch(inode, level ? chain[level - 1] : NULL, offs[level],
                       blks[0], need);
    *blk = blks[need - 1];
out:
    ext2_put_branch(chain, 3);
    ext2_put_branch(fresh, 3);
    if (ret)
        err = ext2_free_branch(inode->i_sb, blks, nr);
    return err ? err : ret;
}

static int ext2_read_file_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct inode *inode = m->m_host;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    uint64_t off = cp->index * PAGE_SIZE;
    size_t len = MIN((uint64_t)inode->i_size - off, (uint64_t)PAGE_SIZE);
    char *data = mapping_data(cp);
    uint32_t blk;
    uint64_t sect;
    int ret;

    ret = ext2_bmap(inode, (uint32_t)cp->index, &blk);
    if (ret)
        return ret;
    if (!blk) {
        memset(data, 0, PAGE_SIZE);
        return 0;
    }
    if (blk >= sbi->s_es->s_blocks_count)
        return -EIO;

    sect = (uint64_t)blk * PAGE_SIZE / BLK_SECTOR_SIZE;
    ret = blk_read(m->m_bdev, sect, cp->page, 0, PAGE_SIZE);
    if (ret)
        return ret;

    memset(data + len, 0, PAGE_SIZE - len);
    return 0;
}

static int ext2_write_file_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct inode *inode = m->m_host;
    uint32_t blk;
    uint64_t sect;
    int ret;

    ret = ext2_bmap(inode, (uint32_t)cp->index, &blk);
    if (ret)
        return ret;
    Assert(blk, "writeback requires an allocated file block\n");

    sect = (uint64_t)blk * PAGE_SIZE / BLK_SECTOR_SIZE;
    return blk_write(m->m_bdev, sect, cp->page, 0, PAGE_SIZE);
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
    .write_page = ext2_write_file_page,
};

static void ext2_evict_inode(struct inode *inode)
{
    struct ext2_inode_info *info = ext2_i(inode);

    mutex_lock(&inode->mutex);
    mapping_purge(&info->mapping, 0, UINT64_MAX);
    mutex_unlock(&inode->mutex);
}

static void ext2_destroy_inode(struct inode *inode)
{
    kfree(ext2_i(inode));
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

/* span is the number of logical data blocks covered by this branch. */
static int ext2_trim_branch(struct inode *inode, uint32_t *block,
                            unsigned int depth, uint64_t first, uint64_t span,
                            uint64_t keep)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct cache_page *cp __free(cache_page) = NULL;
    uint32_t ptrs = sbi->block_size / sizeof(uint32_t);
    int ret;

    assert_mutex_locked(&inode->mutex);
    if (!*block || first + span <= keep)
        return 0;

    if (depth) {
        uint32_t *data;
        bool empty = true;

        cp = mapping_get(&sbi->mapping, *block);
        if (IS_ERR(cp))
            return PTR_ERR(move(cp));
        mutex_lock(&cp->mutex);
        data = mapping_data(cp);
        for (uint32_t i = 0; i < ptrs; i++) {
            uint32_t old = data[i];

            ret =
                ext2_trim_branch(inode, &data[i], depth - 1,
                                 first + i * (span / ptrs), span / ptrs, keep);
            if (data[i] != old)
                mapping_dirty(cp);
            if (ret) {
                mutex_unlock(&cp->mutex);
                return ret;
            }
            if (data[i])
                empty = false;
        }
        mutex_unlock(&cp->mutex);
        if (!empty)
            return 0;
    }

    ret = ext2_free_block(inode->i_sb, *block);
    if (ret)
        return ret;
    *block = 0;
    inode->i_blocks -= sbi->block_size / BLK_SECTOR_SIZE;
    return 0;
}

static int ext2_truncate_blocks(struct inode *inode, uint64_t keep)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    uint32_t *data = ext2_i(inode)->data;
    uint64_t first = EXT2_NDIR_BLOCKS;
    uint64_t span = 1;
    uint32_t ptrs = sbi->block_size / sizeof(uint32_t);
    int ret;

    assert_mutex_locked(&inode->mutex);
    for (unsigned int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        ret = ext2_trim_branch(inode, &data[i], 0, i, 1, keep);
        if (ret)
            return ret;
    }
    for (unsigned int i = EXT2_IND_BLOCK; i < EXT2_N_BLOCKS; i++) {
        span *= ptrs;
        ret = ext2_trim_branch(inode, &data[i], i - EXT2_IND_BLOCK + 1, first,
                               span, keep);
        if (ret)
            return ret;
        first += span;
    }
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
    info->mapping = MAPPING_INIT(sb->s_bdev, ext2_file_mapping_ops, inode);
    info->mapping.mutex = &inode->mutex;
    return inode;
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
    ret = ext2_read_raw_inode(sb, ino, &raw);
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
        inode->i_fops = &ext2_dir_ops;
    else if (S_ISREG(inode->i_mode))
        inode->i_fops = &ext2_file_ops;
    icache_ready(inode);
    return inode;
}

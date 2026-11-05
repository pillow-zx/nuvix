#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>
#include <nuvix/string.h>

#include "ext2.h"

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

int ext2_truncate_blocks(struct inode *inode, uint64_t keep)
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

static void ext2_free_inode_branch(struct inode *inode, uint32_t blk,
                                   unsigned int depth)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct cache_page *cp __free(cache_page) = NULL;
    int ret;

    if (!blk)
        return;
    if (depth) {
        uint32_t *data;

        cp = mapping_get(&sbi->mapping, blk);
        if (IS_ERR(cp)) {
            ret = PTR_ERR(move(cp));
            pr_err("ext2: inode %u indirect block %u read failed (%d)\n",
                   (uint32_t)inode->i_ino, blk, ret);
            return;
        }
        mutex_lock(&cp->mutex);
        data = mapping_data(cp);
        for (uint32_t i = 0; i < sbi->block_size / sizeof(*data); i++) {
            uint32_t child = data[i];

            if (!child)
                continue;
            data[i] = 0;
            mapping_dirty(cp);
            ext2_free_inode_branch(inode, child, depth - 1);
        }
        mutex_unlock(&cp->mutex);
    }
    ret = ext2_free_block(inode->i_sb, blk);
    if (ret)
        pr_err("ext2: inode %u block %u free failed (%d)\n",
               (uint32_t)inode->i_ino, blk, ret);
}

void ext2_free_blocks(struct inode *inode)
{
    struct ext2_inode_info *info = ext2_i(inode);

    for (unsigned int i = 0; i < EXT2_N_BLOCKS; i++) {
        uint32_t blk = info->data[i];
        unsigned int depth = i < EXT2_NDIR_BLOCKS ? 0 : i - EXT2_IND_BLOCK + 1;

        info->data[i] = 0;
        ext2_free_inode_branch(inode, blk, depth);
    }
}

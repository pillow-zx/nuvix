#include <nuvix/bitmap.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/page.h>

#include "ext2.h"

struct cache_page *ext2_get_group_desc(struct ext2_sb_info *sbi, uint32_t grp,
                                       uint32_t *off)
{
    uint32_t nr = sbi->block_size / sizeof(struct ext2_group_desc);
    uint32_t blk = sbi->s_es->s_first_data_block + 1 + grp / nr;

    *off = (grp % nr) * sizeof(struct ext2_group_desc);
    return mapping_get(&sbi->mapping, blk);
}

static int ext2_check_params(struct ext2_sb_info *sbi)
{
    struct ext2_super_block *es = sbi->s_es;

    if (es->s_first_data_block >= es->s_blocks_count ||
        es->s_blocks_per_group > sbi->block_size * 8)
        return -EUCLEAN;
    if (!es->s_free_blocks_count)
        return -ENOSPC;

    return 0;
}

static void ext2_update_free(struct ext2_sb_info *sbi, struct cache_page *dp,
                             uint32_t off, int delta)
{
    struct ext2_group_desc *desc;

    mutex_lock(&dp->mutex);
    desc = (struct ext2_group_desc *)((char *)mapping_data(dp) + off);
    desc->bg_free_blocks_count += delta;
    mapping_dirty(dp);
    mutex_unlock(&dp->mutex);

    mutex_lock(&sbi->s_sbh->mutex);
    sbi->s_es->s_free_blocks_count += delta;
    mapping_dirty(sbi->s_sbh);
    mutex_unlock(&sbi->s_sbh->mutex);
}

static int ext2_alloc_group_block(struct ext2_sb_info *sbi,
                                  struct cache_page *dp, uint32_t off,
                                  uint64_t first, uint32_t bblk, uint32_t *blk)
{
    struct cache_page *bp __free(cache_page) = NULL;
    uint32_t nr = MIN((uint64_t)sbi->s_es->s_blocks_per_group,
                      (uint64_t)sbi->s_es->s_blocks_count - first);
    unsigned long bit;

    bp = mapping_get(&sbi->mapping, bblk);
    if (IS_ERR(bp))
        return PTR_ERR(move(bp));

    mutex_lock(&bp->mutex);
    bit = find_first_zero_bit(mapping_data(bp), nr);
    if (bit == nr || first + bit == 0) {
        mutex_unlock(&bp->mutex);
        return -EUCLEAN;
    }
    set_bit(bit, mapping_data(bp));
    mapping_dirty(bp);
    mutex_unlock(&bp->mutex);
    ext2_update_free(sbi, dp, off, -1);
    *blk = first + bit;
    return 0;
}

int ext2_alloc_block(struct super_block *sb, uint32_t *blk)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct ext2_super_block *es = sbi->s_es;
    uint64_t first;
    uint32_t grp;
    int ret = -ENOSPC;

    mutex_lock(&sbi->mutex);
    ret = ext2_check_params(sbi);
    if (ret)
        goto out;

    for (grp = 0, first = es->s_first_data_block; first < es->s_blocks_count;
         grp++, first += es->s_blocks_per_group) {
        struct cache_page *dp __free(cache_page) = NULL;
        struct ext2_group_desc *desc;
        uint32_t off;
        uint32_t bblk;
        bool avail;

        dp = ext2_get_group_desc(sbi, grp, &off);
        if (IS_ERR(dp)) {
            ret = PTR_ERR(move(dp));
            goto out;
        }
        mutex_lock(&dp->mutex);
        desc = (struct ext2_group_desc *)((char *)mapping_data(dp) + off);
        avail = desc->bg_free_blocks_count != 0;
        bblk = desc->bg_block_bitmap;
        mutex_unlock(&dp->mutex);
        if (!avail)
            continue;

        ret = ext2_alloc_group_block(sbi, dp, off, first, bblk, blk);
        break;
    }
out:
    mutex_unlock(&sbi->mutex);
    return ret;
}

int ext2_free_block(struct super_block *sb, uint32_t blk)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct ext2_super_block *es = sbi->s_es;
    struct cache_page *dp __free(cache_page) = NULL;
    struct cache_page *bp __free(cache_page) = NULL;
    struct ext2_group_desc *desc;
    uint32_t grp;
    uint32_t off;
    uint32_t bit;
    uint32_t bblk;
    int ret = 0;

    mutex_lock(&sbi->mutex);
    Assert(blk && blk >= es->s_first_data_block && blk < es->s_blocks_count,
           "freeing a block outside the filesystem\n");
    grp = (blk - es->s_first_data_block) / es->s_blocks_per_group;
    bit = (blk - es->s_first_data_block) % es->s_blocks_per_group;
    dp = ext2_get_group_desc(sbi, grp, &off);
    if (IS_ERR(dp)) {
        ret = PTR_ERR(move(dp));
        goto out;
    }
    mutex_lock(&dp->mutex);
    desc = (struct ext2_group_desc *)((char *)mapping_data(dp) + off);
    bblk = desc->bg_block_bitmap;
    mutex_unlock(&dp->mutex);

    bp = mapping_get(&sbi->mapping, bblk);
    if (IS_ERR(bp)) {
        ret = PTR_ERR(move(bp));
        goto out;
    }
    mapping_purge(&sbi->mapping, blk, (uint64_t)blk + 1);
    mutex_lock(&bp->mutex);
    Assert(test_bit(bit, mapping_data(bp)), "freeing an unused block\n");
    clear_bit(bit, mapping_data(bp));
    mapping_dirty(bp);
    mutex_unlock(&bp->mutex);
    ext2_update_free(sbi, dp, off, 1);
out:
    mutex_unlock(&sbi->mutex);
    return ret;
}

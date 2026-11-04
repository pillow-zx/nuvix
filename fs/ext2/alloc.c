#include <nuvix/bitmap.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/page.h>

#include "ext2.h"

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

static void ext2_update_free(struct ext2_sb_info *sbi,
                             struct cache_page *desc_page, uint32_t group,
                             int change)
{
    uint32_t per_block = sbi->block_size / sizeof(struct ext2_group_desc);
    struct ext2_group_desc *desc;

    mutex_lock(&desc_page->mutex);
    desc =
        (struct ext2_group_desc *)mapping_data(desc_page) + group % per_block;
    desc->bg_free_blocks_count += change;
    mapping_dirty(desc_page);
    mutex_unlock(&desc_page->mutex);

    mutex_lock(&sbi->s_sbh->mutex);
    sbi->s_es->s_free_blocks_count += change;
    mapping_dirty(sbi->s_sbh);
    mutex_unlock(&sbi->s_sbh->mutex);
}

int ext2_alloc_block(struct super_block *sb, uint32_t *block)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct ext2_super_block *es = sbi->s_es;
    uint32_t per_block = sbi->block_size / sizeof(struct ext2_group_desc);
    uint64_t first;
    uint32_t group;
    int ret = -ENOSPC;

    mutex_lock(&sbi->mutex);
    ret = ext2_check_params(sbi);
    if (ret)
        goto out;

    for (group = 0, first = es->s_first_data_block; first < es->s_blocks_count;
         group++, first += es->s_blocks_per_group) {
        struct cache_page *desc_page __free(cache_page) = NULL;
        struct cache_page *bitmap __free(cache_page) = NULL;
        struct ext2_group_desc *desc;
        uint32_t bitmap_block;
        uint32_t count = MIN((uint64_t)es->s_blocks_per_group,
                             (uint64_t)es->s_blocks_count - first);
        unsigned long bit;
        bool available;

        desc_page = mapping_get(&sbi->mapping,
                                es->s_first_data_block + 1 + group / per_block);
        if (IS_ERR(desc_page)) {
            ret = PTR_ERR(move(desc_page));
            goto out;
        }
        mutex_lock(&desc_page->mutex);
        desc = (struct ext2_group_desc *)mapping_data(desc_page) +
               group % per_block;
        available = desc->bg_free_blocks_count != 0;
        bitmap_block = desc->bg_block_bitmap;
        mutex_unlock(&desc_page->mutex);
        if (!available)
            continue;

        bitmap = mapping_get(&sbi->mapping, bitmap_block);
        if (IS_ERR(bitmap)) {
            ret = PTR_ERR(move(bitmap));
            goto out;
        }
        mutex_lock(&bitmap->mutex);
        bit = find_first_zero_bit(mapping_data(bitmap), count);
        if (bit == count || first + bit == 0) {
            mutex_unlock(&bitmap->mutex);
            ret = -EUCLEAN;
            goto out;
        }
        set_bit(bit, mapping_data(bitmap));
        mapping_dirty(bitmap);
        mutex_unlock(&bitmap->mutex);
        ext2_update_free(sbi, desc_page, group, -1);
        *block = first + bit;
        ret = 0;
        break;
    }
out:
    mutex_unlock(&sbi->mutex);
    return ret;
}

int ext2_free_block(struct super_block *sb, uint32_t block)
{
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct ext2_super_block *es = sbi->s_es;
    struct cache_page *desc_page __free(cache_page) = NULL;
    struct cache_page *bitmap __free(cache_page) = NULL;
    uint32_t per_block = sbi->block_size / sizeof(struct ext2_group_desc);
    struct ext2_group_desc *desc;
    uint32_t group;
    uint32_t bit;
    uint32_t bitmap_block;
    int ret = 0;

    mutex_lock(&sbi->mutex);
    Assert(block && block >= es->s_first_data_block &&
               block < es->s_blocks_count,
           "freeing a block outside the filesystem\n");
    group = (block - es->s_first_data_block) / es->s_blocks_per_group;
    bit = (block - es->s_first_data_block) % es->s_blocks_per_group;
    desc_page = mapping_get(&sbi->mapping,
                            es->s_first_data_block + 1 + group / per_block);
    if (IS_ERR(desc_page)) {
        ret = PTR_ERR(move(desc_page));
        goto out;
    }
    mutex_lock(&desc_page->mutex);
    desc =
        (struct ext2_group_desc *)mapping_data(desc_page) + group % per_block;
    bitmap_block = desc->bg_block_bitmap;
    mutex_unlock(&desc_page->mutex);

    bitmap = mapping_get(&sbi->mapping, bitmap_block);
    if (IS_ERR(bitmap)) {
        ret = PTR_ERR(move(bitmap));
        goto out;
    }
    mutex_lock(&bitmap->mutex);
    Assert(test_bit(bit, mapping_data(bitmap)), "freeing an unused block\n");
    clear_bit(bit, mapping_data(bitmap));
    mapping_dirty(bitmap);
    mutex_unlock(&bitmap->mutex);
    ext2_update_free(sbi, desc_page, group, 1);
out:
    mutex_unlock(&sbi->mutex);
    return ret;
}

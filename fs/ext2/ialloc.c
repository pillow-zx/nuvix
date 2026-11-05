#include <nuvix/bitmap.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/printk.h>
#include <nuvix/string.h>

#include "ext2.h"

static void ext2_update_free_inodes(struct ext2_sb_info *sbi,
                                    struct cache_page *dp, uint32_t off,
                                    bool dir, int delta)
{
    struct ext2_group_desc *desc;

    mutex_lock(&dp->mutex);
    desc = (struct ext2_group_desc *)((char *)mapping_data(dp) + off);
    desc->bg_free_inodes_count += delta;
    if (dir)
        desc->bg_used_dirs_count -= delta;
    mapping_dirty(dp);
    mutex_unlock(&dp->mutex);

    mutex_lock(&sbi->s_sbh->mutex);
    sbi->s_es->s_free_inodes_count += delta;
    mapping_dirty(sbi->s_sbh);
    mutex_unlock(&sbi->s_sbh->mutex);
}

struct inode *ext2_alloc_inode(struct inode *dir, unsigned int mode)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    struct ext2_super_block *es = sbi->s_es;
    struct cache_page *ip __free(cache_page) = NULL;
    struct inode *inode __free(iput) = NULL;
    uint32_t first_ino =
        es->s_rev_level ? es->s_first_ino : EXT2_GOOD_OLD_FIRST_INO;
    uint32_t ioff = 0;
    uint64_t first;
    uint32_t grp;
    int ret = -ENOSPC;

    inode = i_alloc(dir->i_sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    inode->i_mode = (mode & S_IFMT) | 0777;
    inode->i_nlink = 1;
    if (S_ISDIR(mode))
        inode->i_fops = &ext2_dir_operations;
    else if (S_ISREG(mode))
        inode->i_fops = &ext2_file_operations;

    mutex_lock(&inode->mutex);
    mutex_lock(&sbi->mutex);
    if (!es->s_free_inodes_count)
        goto out;
    for (grp = 0, first = 0; first < es->s_inodes_count;
         grp++, first += es->s_inodes_per_group) {
        struct cache_page *dp __free(cache_page) = NULL;
        struct cache_page *bp __free(cache_page) = NULL;
        struct ext2_group_desc *desc;
        uint32_t nr = MIN((uint64_t)es->s_inodes_per_group,
                          (uint64_t)es->s_inodes_count - first);
        uint32_t start = first < first_ino - 1 ? first_ino - 1 - first : 0;
        uint32_t off;
        uint32_t bblk;
        unsigned long bit;
        bool avail;

        if (start >= nr)
            continue;
        dp = ext2_get_grp_desc(sbi, grp, &off);
        if (IS_ERR(dp)) {
            ret = PTR_ERR(move(dp));
            goto out;
        }
        mutex_lock(&dp->mutex);
        desc = (struct ext2_group_desc *)((char *)mapping_data(dp) + off);
        avail = desc->bg_free_inodes_count != 0;
        bblk = desc->bg_inode_bitmap;
        mutex_unlock(&dp->mutex);
        if (!avail)
            continue;
        bp = mapping_get(&sbi->mapping, bblk);
        if (IS_ERR(bp)) {
            ret = PTR_ERR(move(bp));
            goto out;
        }
        mutex_lock(&bp->mutex);
        bit = find_next_zero_bit(mapping_data(bp), nr, start);
        mutex_unlock(&bp->mutex);
        if (bit == nr) {
            ret = -EUCLEAN;
            goto out;
        }
        inode->i_ino = first + bit + 1;
        ip = ext2_get_inode_page(inode, &ioff);
        if (IS_ERR(ip)) {
            ret = PTR_ERR(move(ip));
            goto out;
        }
        mutex_lock(&bp->mutex);
        set_bit(bit, mapping_data(bp));
        mapping_dirty(bp);
        mutex_unlock(&bp->mutex);
        ext2_update_free_inodes(sbi, dp, off, S_ISDIR(mode), -1);
        ret = 0;
        break;
    }
out:
    mutex_unlock(&sbi->mutex);
    mutex_unlock(&inode->mutex);
    if (ret)
        return ERR_PTR(ret);

    ret = icache_inserts(inode);
    if (ret) {
        pr_err("ext2: allocated inode %u is already cached (%d)\n",
               (uint32_t)inode->i_ino, ret);
        return ERR_PTR(ret);
    }
    mutex_lock(&inode->mutex);
    mutex_lock(&ip->mutex);
    memset((char *)mapping_data(ip) + ioff, 0, sbi->inode_size);
    mutex_unlock(&ip->mutex);
    ext2_write_inode(inode, ip, ioff);
    mutex_unlock(&inode->mutex);
    return move(inode);
}

void ext2_free_inode(struct inode *inode)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct ext2_super_block *es = sbi->s_es;
    struct cache_page *dp __free(cache_page) = NULL;
    struct cache_page *bp __free(cache_page) = NULL;
    struct ext2_group_desc *desc;
    uint32_t grp = (inode->i_ino - 1) / es->s_inodes_per_group;
    uint32_t bit = (inode->i_ino - 1) % es->s_inodes_per_group;
    uint32_t off;
    uint32_t bblk;
    int ret = 0;

    mutex_lock(&sbi->mutex);
    dp = ext2_get_grp_desc(sbi, grp, &off);
    if (IS_ERR(dp)) {
        ret = PTR_ERR(move(dp));
        goto out;
    }
    mutex_lock(&dp->mutex);
    desc = (struct ext2_group_desc *)((char *)mapping_data(dp) + off);
    bblk = desc->bg_inode_bitmap;
    mutex_unlock(&dp->mutex);
    bp = mapping_get(&sbi->mapping, bblk);
    if (IS_ERR(bp)) {
        ret = PTR_ERR(move(bp));
        goto out;
    }
    mutex_lock(&bp->mutex);
    clear_bit(bit, mapping_data(bp));
    mapping_dirty(bp);
    mutex_unlock(&bp->mutex);
    ext2_update_free_inodes(sbi, dp, off, S_ISDIR(inode->i_mode), 1);
out:
    mutex_unlock(&sbi->mutex);
    if (ret)
        pr_err("ext2: inode %u free failed (%d)\n", (uint32_t)inode->i_ino,
               ret);
}

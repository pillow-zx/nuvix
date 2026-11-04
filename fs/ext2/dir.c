#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/page.h>
#include <nuvix/string.h>

#include "ext2.h"

static const struct ext2_dir_entry *ext2_get_dirent(struct ext2_sb_info *sbi,
                                                    const char *buf,
                                                    uint32_t off, uint32_t end)
{
    const struct ext2_dir_entry *de;

    if (end - off < sizeof(*de))
        return ERR_PTR(-EUCLEAN);
    de = (const struct ext2_dir_entry *)(buf + off);
    if (de->rec_len < sizeof(*de) || (de->rec_len & 3) ||
        de->rec_len > end - off || de->name_len > de->rec_len - sizeof(*de))
        return ERR_PTR(-EUCLEAN);
    if (de->inode > sbi->s_es->s_inodes_count)
        return ERR_PTR(-EUCLEAN);
    return de;
}

/* Returns NULL for a hole and an unlocked page reference otherwise. */
static struct cache_page *ext2_get_dir_page(struct inode *dir, uint32_t lbn)
{
    uint32_t blk;
    int ret;

    ret = ext2_bmap(dir, lbn, &blk);
    if (ret)
        return ERR_PTR(ret);
    if (!blk)
        return NULL;
    return mapping_get(&ext2_sb_info(dir->i_sb)->mapping, blk);
}

static int ext2_find_entry(struct ext2_sb_info *sbi, const char *buf,
                           uint32_t end, const char *name, uint32_t len,
                           uint32_t *ino)
{
    uint32_t off = 0;

    while (off < end) {
        const struct ext2_dir_entry *de;

        de = ext2_get_dirent(sbi, buf, off, end);
        if (IS_ERR(de))
            return PTR_ERR(de);
        if (de->inode && de->name_len == len && !memcmp(de->name, name, len)) {
            *ino = de->inode;
            return 0;
        }
        off += de->rec_len;
    }
    return -ENOENT;
}

int ext2_inode_by_name(struct inode *dir, const char *name, uint32_t len,
                       uint32_t *ino)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    uint64_t off;

    assert_mutex_locked(&dir->mutex);
    for (off = 0; off < (uint64_t)dir->i_size; off += sbi->block_size) {
        struct cache_page *cp __free(cache_page) = NULL;
        uint64_t left = (uint64_t)dir->i_size - off;
        uint32_t end = left > sbi->block_size ? sbi->block_size : left;
        int ret;

        cp = ext2_get_dir_page(dir, off / sbi->block_size);
        if (IS_ERR(cp))
            return PTR_ERR(move(cp));
        if (!cp)
            continue;
        mutex_lock(&cp->mutex);
        ret = ext2_find_entry(sbi, mapping_data(cp), end, name, len, ino);
        mutex_unlock(&cp->mutex);
        if (ret != -ENOENT)
            return ret;
    }
    return -ENOENT;
}

static ssize_t ext2_dir_read(struct kiocb *iocb, struct kiov *iov)
{
    (void)iocb;
    (void)iov;
    return -EISDIR;
}

static const unsigned char ext2_dtype[EXT2_FT_MAX] = {
    [EXT2_FT_UNKNOWN] = DT_UNKNOWN, [EXT2_FT_REG_FILE] = DT_REG,
    [EXT2_FT_DIR] = DT_DIR,         [EXT2_FT_CHRDEV] = DT_CHR,
    [EXT2_FT_BLKDEV] = DT_BLK,      [EXT2_FT_FIFO] = DT_FIFO,
    [EXT2_FT_SOCK] = DT_SOCK,       [EXT2_FT_SYMLINK] = DT_LNK,
};

/* The caller holds the directory mutex; 0 means a hole, 1 means data. */
static int ext2_read_dir_block(struct inode *inode, uint32_t lbn, char *buf,
                               uint32_t end)
{
    struct cache_page *cp __free(cache_page) = NULL;

    cp = ext2_get_dir_page(inode, lbn);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    if (!cp)
        return 0;
    mutex_lock(&cp->mutex);
    memcpy(buf, mapping_data(cp), end);
    mutex_unlock(&cp->mutex);
    return 1;
}

/* Returns 1 after consuming the block, 0 when dir_emit stops. */
static int ext2_readdir_block(struct ext2_sb_info *sbi, const char *buf,
                              uint32_t end, struct dirctx *ctx)
{
    uint32_t off = ctx->pos % sbi->block_size;

    while (off < end) {
        const struct ext2_dir_entry *de;
        loff_t next;

        de = ext2_get_dirent(sbi, buf, off, end);
        if (IS_ERR(de))
            return PTR_ERR(de);
        next = ctx->pos + de->rec_len;
        if (de->inode) {
            unsigned char type = DT_UNKNOWN;

            if ((sbi->s_es->s_feature_incompat &
                 EXT2_FEATURE_INCOMPAT_FILETYPE) &&
                de->file_type < EXT2_FT_MAX)
                type = ext2_dtype[de->file_type];
            if (!dir_emit(ctx, de->name, de->name_len, next, de->inode, type))
                return 0;
        }
        ctx->pos = next;
        off += de->rec_len;
    }
    return 1;
}

static int ext2_dir_readdir(struct file *file, struct dirctx *ctx)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct page *page __free(page) = NULL;
    char *buf;

    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;
    page = alloc_page();
    if (!page)
        return -ENOMEM;
    buf = (char *)__va(page_to_phys(page));

    for (;;) {
        uint32_t lbn;
        uint32_t off;
        uint64_t left;
        uint32_t end;
        int ret;

        mutex_lock(&inode->mutex);
        if (ctx->pos >= inode->i_size) {
            mutex_unlock(&inode->mutex);
            return 0;
        }
        lbn = ctx->pos / sbi->block_size;
        off = ctx->pos % sbi->block_size;
        left = inode->i_size - ctx->pos;
        end = off + left > sbi->block_size ? sbi->block_size : off + left;
        ret = ext2_read_dir_block(inode, lbn, buf, end);
        if (!ret)
            ctx->pos = ((loff_t)lbn + 1) * sbi->block_size;
        mutex_unlock(&inode->mutex);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        ret = ext2_readdir_block(sbi, buf, end, ctx);
        if (ret <= 0)
            return ret;
    }
}

const struct file_operations ext2_dir_ops = {
    .llseek = ext2_llseek,
    .read = ext2_dir_read,
    .readdir = ext2_dir_readdir,
};

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/string.h>

#include "ext2.h"

struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct ext2_sb_info *sbi = ext2_sb_info(sb);
    struct inode *inode;
    uint64_t offset;
    uint32_t block_size = sbi->block_size;
    uint32_t ino;
    int ret;

    if (!S_ISDIR(dir->i_mode))
        return ERR_PTR(-ENOTDIR);
    if (dentry->d_nlen > VFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);

    for (offset = 0; (uint64_t)dir->i_size > offset; offset += block_size) {
        struct cache_page *cp __free(cache_page) = NULL;
        const struct ext2_dir_entry *de;
        const char *buffer;
        uint64_t remain = (uint64_t)dir->i_size - offset;
        uint32_t limit = remain > block_size ? block_size : (uint32_t)remain;
        uint32_t logical = (uint32_t)(offset / block_size);
        uint32_t pos = 0;
        uint32_t phys;

        ret = ext2_bmap(dir, logical, &phys);
        if (ret)
            return ERR_PTR(ret);
        if (!phys)
            continue;

        cp = mapping_get(&sbi->mapping, phys);
        if (IS_ERR(cp))
            return ERR_PTR(PTR_ERR(move(cp)));
        buffer = mapping_data(cp);

        while (pos < limit) {
            uint16_t rec_len;

            if (limit - pos < sizeof(*de))
                return ERR_PTR(-EUCLEAN);
            de = (const struct ext2_dir_entry *)(buffer + pos);
            rec_len = de->rec_len;
            if (rec_len < sizeof(*de) || (rec_len & 3) || rec_len > limit - pos)
                return ERR_PTR(-EUCLEAN);
            if (de->name_len > rec_len - sizeof(*de))
                return ERR_PTR(-EUCLEAN);
            if (de->inode) {
                if (de->inode > sbi->s_es->s_inodes_count)
                    return ERR_PTR(-EUCLEAN);
                if (de->name_len == dentry->d_nlen &&
                    !memcmp(de->name, dentry->d_name, dentry->d_nlen)) {
                    ino = de->inode;
                    goto found;
                }
            }
            pos += rec_len;
        }
        if (pos != limit)
            return ERR_PTR(-EUCLEAN);
    }

    return dentry;

found:
    inode = ext2_iget(sb, ino);
    if (IS_ERR(inode))
        return ERR_PTR(PTR_ERR(inode));

    dentry->d_inode = inode;
    return dentry;
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

static int ext2_dir_readdir(struct file *file, struct dirctx *ctx)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    struct super_block *sb = inode->i_sb;
    struct ext2_sb_info *sbi = ext2_sb_info(sb);

    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    while (ctx->pos < inode->i_size) {
        struct cache_page *cp __free(cache_page) = NULL;
        const char *buffer;
        uint32_t logical = (uint32_t)(ctx->pos / sbi->block_size);
        uint32_t offset = (uint32_t)(ctx->pos % sbi->block_size);
        uint64_t remain = (uint64_t)(inode->i_size - ctx->pos);
        uint32_t limit = offset + remain > sbi->block_size
                             ? sbi->block_size
                             : (uint32_t)(offset + remain);
        uint32_t phys;
        int ret;

        ret = ext2_bmap(inode, logical, &phys);
        if (ret)
            return ret;
        if (!phys) {
            ctx->pos = ((loff_t)logical + 1) * sbi->block_size;
            continue;
        }

        cp = mapping_get(&sbi->mapping, phys);
        if (IS_ERR(cp))
            return PTR_ERR(move(cp));
        buffer = mapping_data(cp);

        while (offset < limit) {
            const struct ext2_dir_entry *de;
            uint16_t rec_len;
            loff_t next;

            if (limit - offset < sizeof(*de))
                return -EUCLEAN;
            de = (const struct ext2_dir_entry *)(buffer + offset);
            rec_len = de->rec_len;
            if (rec_len < sizeof(*de) || (rec_len & 3) ||
                rec_len > limit - offset)
                return -EUCLEAN;
            if (de->name_len > rec_len - sizeof(*de))
                return -EUCLEAN;

            next = (loff_t)logical * sbi->block_size + offset + rec_len;
            if (de->inode) {
                unsigned char type = DT_UNKNOWN;

                if (de->inode > sbi->s_es->s_inodes_count)
                    return -EUCLEAN;
                if ((sbi->s_es->s_feature_incompat &
                     EXT2_FEATURE_INCOMPAT_FILETYPE) &&
                    de->file_type < EXT2_FT_MAX)
                    type = ext2_dtype[de->file_type];
                if (!dir_emit(ctx, de->name, de->name_len, next, de->inode,
                              type))
                    return 0;
            }

            ctx->pos = next;
            offset += rec_len;
        }
    }

    return 0;
}

const struct file_operations ext2_dir_ops = {
    .llseek = ext2_llseek,
    .read = ext2_dir_read,
    .readdir = ext2_dir_readdir,
};

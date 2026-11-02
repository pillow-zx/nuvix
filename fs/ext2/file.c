#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>

#include "ext2.h"

static int ext2_file_open(struct file *file)
{
    (void)file;
    return 0;
}

static ssize_t ext2_file_read(struct kiocb *iocb, struct kiov *iov)
{
    struct inode *inode = iocb->file->f_path.dentry->d_inode;
    struct super_block *sb = inode->i_sb;
    struct ext2_fs *fs = ext2_sb_info(sb);
    char *buffer __free(kfree) = NULL;
    loff_t pos = iocb->pos;
    size_t count;
    size_t done = 0;
    int ret = 0;

    if (pos < 0)
        return -EINVAL;
    if (pos >= inode->i_size)
        return 0;

    count = MIN(iov->count, (size_t)(inode->i_size - pos));
    if (!count)
        return 0;

    buffer = kmalloc(fs->block_size);
    if (!buffer)
        return -ENOMEM;

    while (done < count) {
        uint32_t logical = (uint32_t)(pos / fs->block_size);
        size_t offset = (size_t)(pos % fs->block_size);
        size_t chunk = MIN((size_t)fs->block_size - offset, count - done);
        uint32_t phys;
        size_t copied;

        ret = ext2_bmap(inode, logical, &phys);
        if (ret)
            break;

        if (phys) {
            ret = ext2_read_block(sb, phys, buffer);
            if (ret)
                break;
            copied = copy_to_kiov(buffer + offset, chunk, iov);
        } else {
            memset(buffer, 0, chunk);
            copied = copy_to_kiov(buffer, chunk, iov);
        }

        done += copied;
        pos += copied;
        iocb->pos = pos;
        if (copied != chunk) {
            ret = -EFAULT;
            break;
        }
    }

    return done ? (ssize_t)done : ret;
}

static ssize_t ext2_file_write(struct kiocb *iocb, struct kiov *iov)
{
    (void)iocb;
    (void)iov;
    return -EROFS;
}

static int ext2_file_fsync(struct file *file)
{
    (void)file;
    return 0;
}

static int ext2_file_release(struct file *file)
{
    (void)file;
    return 0;
}

static loff_t ext2_llseek(struct file *file, loff_t offset, int whence)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    loff_t pos = 0;
    int ret = 0;

    mutex_lock(&file->f_pos_lock);
    switch (whence) {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        if (check_add_overflow(file->f_pos, offset, &pos))
            ret = -EOVERFLOW;
        break;
    case SEEK_END:
        if (check_add_overflow(inode->i_size, offset, &pos))
            ret = -EOVERFLOW;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    if (!ret && pos < 0)
        ret = -EINVAL;
    if (!ret)
        file->f_pos = pos;
    mutex_unlock(&file->f_pos_lock);

    return ret ? ret : pos;
}

const struct file_operations ext2_file_ops = {
    .open = ext2_file_open,
    .llseek = ext2_llseek,
    .read = ext2_file_read,
    .write = ext2_file_write,
    .fsync = ext2_file_fsync,
    .release = ext2_file_release,
};

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

static int ext2_readdir(struct file *file, struct dirctx *ctx)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    struct super_block *sb = inode->i_sb;
    struct ext2_fs *fs = ext2_sb_info(sb);

    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    while (ctx->pos < inode->i_size) {
        struct cache_page *cp __free(cache_page) = NULL;
        const char *buffer;
        uint32_t logical = (uint32_t)(ctx->pos / fs->block_size);
        uint32_t offset = (uint32_t)(ctx->pos % fs->block_size);
        uint64_t remain = (uint64_t)(inode->i_size - ctx->pos);
        uint32_t limit = offset + remain > fs->block_size
                             ? fs->block_size
                             : (uint32_t)(offset + remain);
        uint32_t phys;
        int ret;

        ret = ext2_bmap(inode, logical, &phys);
        if (ret)
            return ret;
        if (!phys) {
            ctx->pos = ((loff_t)logical + 1) * fs->block_size;
            continue;
        }

        cp = mapping_get(&fs->block_mapping, phys);
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

            next = (loff_t)logical * fs->block_size + offset + rec_len;
            if (de->inode) {
                unsigned char type = DT_UNKNOWN;

                if (de->inode > fs->inodes_count)
                    return -EUCLEAN;
                if ((fs->features_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) &&
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
    .readdir = ext2_readdir,
};

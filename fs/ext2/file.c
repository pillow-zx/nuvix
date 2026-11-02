#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/page.h>

#include "ext2.h"

static int ext2_file_open(struct file *file)
{
    (void)file;
    return 0;
}

static ssize_t ext2_file_read(struct kiocb *iocb, struct kiov *iov)
{
    struct inode *inode = iocb->file->f_path.dentry->d_inode;
    struct ext2_inode_info *info = ext2_i(inode);
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

    while (done < count) {
        struct cache_page *cp __free(cache_page) = NULL;
        uint64_t index = (uint64_t)pos / PAGE_SIZE;
        size_t offset = (size_t)(pos % PAGE_SIZE);
        size_t chunk = MIN((size_t)PAGE_SIZE - offset, count - done);
        size_t copied;

        cp = mapping_get(&info->mapping, index);
        if (IS_ERR(cp)) {
            ret = PTR_ERR(move(cp));
            break;
        }
        copied =
            copy_to_kiov((const char *)mapping_data(cp) + offset, chunk, iov);

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

loff_t ext2_llseek(struct file *file, loff_t offset, int whence)
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

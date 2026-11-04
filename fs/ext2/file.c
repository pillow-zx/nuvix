#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/page.h>
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
    struct ext2_inode_info *info = ext2_i(inode);
    struct page *page __free(page) = NULL;
    loff_t pos = iocb->pos;
    size_t count;
    size_t done = 0;
    char *buffer;
    int ret = 0;

    if (pos < 0)
        return -EINVAL;
    mutex_lock(&inode->mutex);
    if (pos >= inode->i_size)
        count = 0;
    else
        count = MIN(iov->count, (size_t)inode->i_size - pos);
    mutex_unlock(&inode->mutex);
    if (!count)
        return 0;

    page = alloc_page();
    if (!page)
        return -ENOMEM;
    buffer = (char *)__va(page_to_phys(page));
    while (done < count) {
        struct cache_page *cp;
        uint64_t index = (uint64_t)pos / PAGE_SIZE;
        size_t offset = (size_t)(pos % PAGE_SIZE);
        size_t chunk = MIN((size_t)PAGE_SIZE - offset, count - done);
        size_t copied;

        mutex_lock(&inode->mutex);
        if (pos >= inode->i_size) {
            mutex_unlock(&inode->mutex);
            break;
        }
        chunk = MIN(chunk, (size_t)(inode->i_size - pos));
        cp = mapping_get(&info->mapping, index);
        if (IS_ERR(cp)) {
            ret = PTR_ERR(cp);
            mutex_unlock(&inode->mutex);
            break;
        }
        mutex_lock(&cp->mutex);
        memcpy(buffer, (const char *)mapping_data(cp) + offset, chunk);
        mutex_unlock(&cp->mutex);
        mapping_put(cp);
        mutex_unlock(&inode->mutex);

        copied = copy_to_kiov(buffer, chunk, iov);
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
    mutex_lock(&inode->mutex);
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
    mutex_unlock(&inode->mutex);
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

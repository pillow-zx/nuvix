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
    struct inode *inode = iocb->file->f_path.dentry->d_inode;
    struct ext2_inode_info *info = ext2_i(inode);
    struct page *page __free(page) = NULL;
    loff_t pos = iocb->pos;
    size_t count = iov->count;
    size_t done = 0;
    char *buffer;
    int ret = 0;

    if (pos < 0)
        return -EINVAL;
    if (!count)
        return 0;
    if ((uint64_t)pos > UINT32_MAX || count > UINT32_MAX - (uint64_t)pos)
        return -EFBIG;
    page = alloc_page();
    if (!page)
        return -ENOMEM;
    buffer = (char *)__va(page_to_phys(page));
    while (done < count) {
        struct cache_page *cp __free(cache_page) = NULL;
        struct cache_page *inode_page __free(cache_page) = NULL;
        struct cache_page *tail __free(cache_page) = NULL;
        uint64_t index = (uint64_t)pos / PAGE_SIZE;
        size_t offset = (size_t)(pos % PAGE_SIZE);
        size_t chunk = MIN((size_t)PAGE_SIZE - offset, count - done);
        size_t copied;
        uint64_t end;
        uint64_t old_blocks;
        uint32_t inode_offset;
        uint32_t block;
        uint32_t tail_block = 0;
        bool changed;

        copied = copy_from_kiov(buffer, chunk, iov);
        if (!copied) {
            ret = -EFAULT;
            break;
        }
        end = (uint64_t)pos + copied;
        mutex_lock(&inode->mutex);
        inode_page = ext2_get_inode_page(inode, &inode_offset);
        if (IS_ERR(inode_page)) {
            ret = PTR_ERR(move(inode_page));
            goto unlock;
        }
        cp = mapping_get(&info->mapping, index);
        if (IS_ERR(cp)) {
            ret = PTR_ERR(move(cp));
            goto unlock;
        }
        if (pos > inode->i_size && inode->i_size % PAGE_SIZE &&
            (uint64_t)inode->i_size / PAGE_SIZE != index) {
            tail = mapping_get(&info->mapping,
                               (uint64_t)inode->i_size / PAGE_SIZE);
            if (IS_ERR(tail)) {
                ret = PTR_ERR(move(tail));
                goto unlock;
            }
            ret = ext2_bmap(inode, (uint64_t)inode->i_size / PAGE_SIZE,
                            &tail_block);
            if (ret)
                goto unlock;
        }
        old_blocks = inode->i_blocks;
        ret = ext2_bmap_create(inode, (uint32_t)index, &block);
        if (ret)
            goto unlock;
        if (tail_block) {
            mutex_lock(&tail->mutex);
            mapping_dirty(tail);
            mutex_unlock(&tail->mutex);
        }
        mutex_lock(&cp->mutex);
        memcpy((char *)mapping_data(cp) + offset, buffer, copied);
        mapping_dirty(cp);
        mutex_unlock(&cp->mutex);
        changed =
            inode->i_blocks != old_blocks || end > (uint64_t)inode->i_size;
        if (end > (uint64_t)inode->i_size)
            inode->i_size = end;
        if (changed)
            ext2_write_inode(inode, inode_page, inode_offset);
    unlock:
        mutex_unlock(&inode->mutex);
        if (ret)
            break;
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

static int ext2_file_fsync(struct file *file)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    int ret;

    mutex_lock(&inode->mutex);
    ret = mapping_flush(&ext2_i(inode)->mapping);
    if (ret)
        goto out;
    ret = mapping_flush(&sbi->mapping);
    if (ret)
        goto out;
    ret = blk_flush(inode->i_sb->s_bdev);
out:
    mutex_unlock(&inode->mutex);
    return ret;
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

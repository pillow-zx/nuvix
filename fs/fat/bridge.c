#include <nuvix/fat.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/mutex.h>
#include <nuvix/vfs.h>
#include <nuvix/ctypes.h>
#include <nuvix/blkdev.h>
#include <nuvix/cleanup.h>
#include <nuvix/page.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include "diskio.h"
#include "ff.h"

static DEFINE_MUTEX(fat_lock);

static ssize_t fat_read(struct kiocb *iocb, struct kiov *iov)
{
    FIL *fil = iocb->file->f_private;
    char *buffer __free(kfree) = NULL;
    size_t done = 0;
    int ret = 0;

    if (iocb->pos < 0)
        return -EINVAL;
    buffer = kmalloc(MIN(iov->count, (size_t)PAGE_SIZE));
    if (!buffer)
        return -ENOMEM;

    while (iov->count) {
        size_t count = MIN(iov->count, (size_t)PAGE_SIZE);
        UINT received = 0;
        size_t copied;

        mutex_lock(&fat_lock);
        if ((uint64_t)iocb->pos > f_size(fil)) {
            mutex_unlock(&fat_lock);
            break;
        }
        ret = f_lseek(fil, (FSIZE_t)iocb->pos);
        if (!ret)
            ret = f_read(fil, buffer, (UINT)count, &received);
        mutex_unlock(&fat_lock);

        copied = copy_to_kiov(buffer, received, iov);
        iocb->pos += copied;
        done += copied;
        if (copied != received)
            ret = -EFAULT;
        if (ret || received < count)
            break;
    }
    return done ? (ssize_t)done : ret;
}

static ssize_t fat_write(struct kiocb *iocb, struct kiov *iov)
{
    FIL *fil = iocb->file->f_private;
    char *buffer __free(kfree) = NULL;
    size_t done = 0;
    int ret = 0;

    if (iocb->pos < 0)
        return -EINVAL;
    if (iocb->pos > UINT32_MAX)
        return -EFBIG;
    buffer = kmalloc(MIN(iov->count, (size_t)PAGE_SIZE));
    if (!buffer)
        return -ENOMEM;

    while (iov->count) {
        struct kiov next = *iov;
        size_t count = MIN(iov->count, (size_t)PAGE_SIZE);
        size_t copied = copy_from_kiov(buffer, count, &next);
        UINT written = 0;

        if (!copied) {
            ret = -EFAULT;
            break;
        }
        mutex_lock(&fat_lock);
        if ((uint64_t)iocb->pos > f_size(fil))
            ret = -EOPNOTSUPP;
        else
            ret = f_lseek(fil, (FSIZE_t)iocb->pos);
        if (!ret)
            ret = f_write(fil, buffer, (UINT)copied, &written);
        mutex_unlock(&fat_lock);

        kiov_advance(iov, written);
        iocb->pos += written;
        done += written;
        if (ret || written < copied)
            break;
        if (copied < count) {
            ret = -EFAULT;
            break;
        }
    }
    return done ? (ssize_t)done : ret;
}

static int fat_fsync(struct file *file)
{
    int ret;

    mutex_lock(&fat_lock);
    ret = f_sync(file->f_private);
    mutex_unlock(&fat_lock);

    return ret;
}

static int fat_release(struct file *file)
{
    FIL *fil = file->f_private;
    int ret;

    mutex_lock(&fat_lock);
    ret = f_close(fil);
    mutex_unlock(&fat_lock);

    kfree(fil);

    return ret;
}

static const struct file_operations fat_file_ops = {
    .read = fat_read,
    .write = fat_write,
    .fsync = fat_fsync,
    .release = fat_release,
};

static ssize_t fat_dir_read(struct kiocb *iocb, struct kiov *iov)
{
    (void)iocb;
    (void)iov;
    return -EISDIR;
}

static int fat_dir_release(struct file *file)
{
    DIR *dir = file->f_private;
    int ret;

    mutex_lock(&fat_lock);
    ret = f_closedir(dir);
    mutex_unlock(&fat_lock);
    kfree(dir);
    return ret;
}

static const struct file_operations fat_dir_ops = {
    .read = fat_dir_read,
    .release = fat_dir_release,
};

int fat_open(struct file *file, const char *path, unsigned long flags)
{
    FIL *fil __free(kfree) = NULL;
    DIR *dir __free(kfree) = NULL;
    FILINFO info;
    BYTE mode;
    int ret;

    if (flags & ~O_ACCMODE)
        return -EOPNOTSUPP;

    switch (flags & O_ACCMODE) {
    case O_RDONLY:
        mode = FA_READ;
        break;
    case O_WRONLY:
        mode = FA_WRITE;
        break;
    case O_RDWR:
        mode = FA_READ | FA_WRITE;
        break;
    default:
        return -EINVAL;
    }

    mutex_lock(&fat_lock);
    ret = f_stat(path, &info);
    if (ret)
        goto out;

    if (info.fattrib & AM_DIR) {
        if ((flags & O_ACCMODE) != O_RDONLY) {
            ret = -EISDIR;
            goto out;
        }
        dir = kzalloc(sizeof(*dir));
        if (!dir) {
            ret = -ENOMEM;
            goto out;
        }
        ret = f_opendir(dir, path);
        if (ret)
            goto out;
        file->f_ops = &fat_dir_ops;
        file->f_private = move(dir);
    } else {
        fil = kzalloc(sizeof(*fil));
        if (!fil) {
            ret = -ENOMEM;
            goto out;
        }
        ret = f_open(fil, path, mode);
        if (ret)
            goto out;
        file->f_ops = &fat_file_ops;
        file->f_private = move(fil);
    }

out:
    mutex_unlock(&fat_lock);
    return ret;
}

int fat_mount(void)
{
    int ret;
    if (!root_bdev)
        return -ENODEV;
    mutex_lock(&fat_lock);
    ret = f_mount();
    mutex_unlock(&fat_lock);

    return ret;
}

int disk_read(BYTE *buff, LBA_t sector, UINT count)
{
    struct blkdev *dev = root_bdev;
    struct page *page __free(page) = NULL;
    uint64_t lba = sector;
    BYTE *data;
    int ret;

    if (!dev)
        return -ENODEV;
    if (!buff || !count)
        return -EINVAL;
    if (lba >= dev->sectors || count > dev->sectors - lba)
        return -EINVAL;

    page = alloc_page();
    if (!page)
        return -ENOMEM;

    data = (BYTE *)__va(page_to_phys(page));

    while (count) {
        UINT n;
        size_t bytes;

        n = MIN(count, (UINT)BLK_MAX_IO_SIZE / BLK_SECTOR_SIZE);
        bytes = (size_t)n * BLK_SECTOR_SIZE;

        ret = blk_read(dev, lba, page, 0, bytes);
        if (ret)
            return ret;

        memcpy(buff, data, bytes);

        buff += bytes;
        lba += n;
        count -= n;
    }

    return 0;
}

int disk_write(const BYTE *buff, LBA_t sector, UINT count)
{
    struct blkdev *dev = root_bdev;
    struct page *page __free(page) = NULL;
    uint64_t lba = sector;
    BYTE *data;
    int ret;

    if (!dev)
        return -ENODEV;
    if (!buff || !count)
        return -EINVAL;
    if (lba >= dev->sectors || count > dev->sectors - lba)
        return -EINVAL;

    page = alloc_page();
    if (!page)
        return -ENOMEM;

    data = (BYTE *)__va(page_to_phys(page));

    while (count) {
        UINT n;
        size_t bytes;

        n = MIN(count, (UINT)BLK_MAX_IO_SIZE / BLK_SECTOR_SIZE);
        bytes = (size_t)n * BLK_SECTOR_SIZE;

        memcpy(data, buff, bytes);

        ret = blk_write(dev, lba, page, 0, bytes);
        if (ret)
            return ret;

        buff += bytes;
        lba += n;
        count -= n;
    }

    return 0;
}

int disk_sync(void)
{
    return blk_flush(root_bdev);
}

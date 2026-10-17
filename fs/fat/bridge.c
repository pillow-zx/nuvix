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

static ssize_t fat_read(struct file *file, char *buff, size_t count,
                        loff_t *pos)
{
    FIL *fil = file->f_private;
    UINT done = 0;
    int ret = 0;

    if (*pos < 0)
        return -EINVAL;
    if (!count)
        return 0;
    if (count > UINT32_MAX)
        count = UINT32_MAX;

    mutex_lock(&fat_lock);

    if ((uint64_t)*pos > f_size(fil))
        goto out;

    ret = f_lseek(fil, (FSIZE_t)*pos);
    if (ret)
        goto out;

    ret = f_read(fil, buff, (UINT)count, &done);
    *pos += done;

out:
    mutex_unlock(&fat_lock);
    return done ? (ssize_t)done : ret;
}

static ssize_t fat_write(struct file *file, const char *buff, size_t count,
                         loff_t *pos)
{
    FIL *fil = file->f_private;
    UINT done = 0;
    int ret;

    if (*pos < 0)
        return -EINVAL;
    if (!count)
        return 0;
    if (*pos > UINT32_MAX)
        return -EFBIG;
    if (count > UINT32_MAX)
        count = UINT32_MAX;

    mutex_lock(&fat_lock);

    if ((uint64_t)*pos > f_size(fil)) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    ret = f_lseek(fil, (FSIZE_t)*pos);
    if (ret)
        goto out;

    ret = f_write(fil, buff, (UINT)count, &done);
    *pos += done;

out:
    mutex_unlock(&fat_lock);
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

int fat_open(struct file *file, const char *path, unsigned long flags)
{
    FIL *fil __free(kfree) = NULL;
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

    fil = kzalloc(sizeof(*fil));
    if (!fil)
        return -ENOMEM;

    mutex_lock(&fat_lock);
    ret = f_open(fil, path, mode);
    mutex_unlock(&fat_lock);

    if (ret)
        return ret;

    file->f_ops = &fat_file_ops;
    file->f_private = move(fil);
    return 0;
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

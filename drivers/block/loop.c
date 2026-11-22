#include <nuvix/blkdev.h>
#include <nuvix/device.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/kiov.h>
#include <nuvix/module.h>
#include <nuvix/page.h>
#include <nuvix/sched.h>
#include <nuvix/string.h>
#include <nuvix/uaccess.h>
#include <nuvix/vfs.h>

#define LOOP_MAJOR         7
#define LOOP_SLOTS         8
#define LOOP_CLR_FD        0x4c01
#define LOOP_GET_STATUS64  0x4c05
#define LOOP_CONFIGURE     0x4c0a

#define LO_FLAGS_READ_ONLY 1U
#define LO_FLAGS_AUTOCLEAR 4U

struct loop_info64 {
    uint64_t lo_device;
    uint64_t lo_inode;
    uint64_t lo_rdevice;
    uint64_t lo_offset;
    uint64_t lo_sizelimit;
    uint32_t lo_number;
    uint32_t lo_encrypt_type;
    uint32_t lo_encrypt_key_size;
    uint32_t lo_flags;
    uint8_t lo_file_name[64];
    uint8_t lo_crypt_name[64];
    uint8_t lo_encrypt_key[32];
    uint64_t lo_init[2];
};

struct loop_config {
    uint32_t fd;
    uint32_t block_size;
    struct loop_info64 info;
    uint64_t reserved[8];
};

static_assert(sizeof(struct loop_info64) == 232, "invalid loop_info64 layout");
static_assert(sizeof(struct loop_config) == 304, "invalid loop_config layout");

struct loop_device {
    struct blkdev bdev;
    struct file *backing;
    struct loop_info64 info;
    unsigned int openers;
    bool claimed;
};

static DEFINE_MUTEX(loop_mutex);
static struct loop_device loops[LOOP_SLOTS];
static const struct blkdev_ops loop_ops;

static struct loop_device *to_loop(struct blkdev *dev)
{
    return container_of(dev, struct loop_device, bdev);
}

static struct file *loop_unbind(struct loop_device *loop)
{
    struct file *backing = loop->backing;

    loop->backing = NULL;
    loop->bdev.sectors = 0;
    return backing;
}

static struct file *loop_autoclear(struct loop_device *loop)
{
    if (loop->backing && !loop->claimed && !loop->openers &&
        (loop->info.lo_flags & LO_FLAGS_AUTOCLEAR))
        return loop_unbind(loop);
    return NULL;
}

static int loop_claim(struct blkdev *dev, unsigned int flags)
{
    struct loop_device *loop = to_loop(dev);
    int ret = 0;

    mutex_lock(&loop_mutex);
    if (!loop->backing)
        ret = -ENXIO;
    else if (loop->claimed)
        ret = -EBUSY;
    else if ((flags & MNT_WRITE) && (loop->info.lo_flags & LO_FLAGS_READ_ONLY))
        ret = -EROFS;
    else
        loop->claimed = true;
    mutex_unlock(&loop_mutex);
    return ret;
}

static void loop_release(struct blkdev *dev)
{
    struct loop_device *loop = to_loop(dev);
    struct file *backing;

    mutex_lock(&loop_mutex);
    loop->claimed = false;
    backing = loop_autoclear(loop);
    mutex_unlock(&loop_mutex);
    fput(backing);
}

static int loop_io(struct blkdev *dev, uint64_t sector, struct page *page,
                   size_t offset, size_t length, bool write)
{
    struct loop_device *loop = to_loop(dev);
    struct iovec vec = {
        .iov_base = (char *)__va(page_to_phys(page)) + offset,
        .iov_len = length,
    };
    struct kiocb iocb = {
        .file = loop->backing,
        .pos = loop->info.lo_offset + sector * BLK_SECTOR_SIZE,
    };
    struct kiov iov;
    ssize_t ret;

    kiov_init(&iov, &vec, 1, length, false);
    ret = write ? vfs_write(&iocb, &iov) : vfs_read(&iocb, &iov);
    if (ret < 0)
        return ret;
    return (size_t)ret == length ? 0 : -EIO;
}

static int loop_read(struct blkdev *dev, uint64_t sector, struct page *page,
                     size_t offset, size_t length)
{
    return loop_io(dev, sector, page, offset, length, false);
}

static int loop_write(struct blkdev *dev, uint64_t sector, struct page *page,
                      size_t offset, size_t length)
{
    return loop_io(dev, sector, page, offset, length, true);
}

static int loop_flush(struct blkdev *dev)
{
    struct file *backing = to_loop(dev)->backing;

    return backing->f_ops->fsync ? backing->f_ops->fsync(backing) : -EOPNOTSUPP;
}

static const struct blkdev_ops loop_ops = {
    .claim = loop_claim,
    .release = loop_release,
    .read = loop_read,
    .write = loop_write,
    .flush = loop_flush,
};

static int loop_configure(struct loop_device *loop, struct file *file,
                          const struct loop_config *config)
{
    struct file *backing __free(fput) = NULL;
    struct inode *inode;
    struct blkdev *dev;
    uint64_t size;
    uint64_t length;
    bool readonly;
    int ret;

    if ((config->block_size && config->block_size != BLK_SECTOR_SIZE) ||
        config->info.lo_encrypt_type || config->info.lo_encrypt_key_size ||
        (config->info.lo_flags & ~(LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR)))
        return -EINVAL;
    backing = fd_open(current->fdtable, config->fd);
    if (!backing)
        return -EBADF;
    inode = backing->f_inode;
    if (!inode || !S_ISREG(inode->i_mode) || !backing->f_ops->read)
        return -EINVAL;
    if (!FILE_READ(backing))
        return -EBADF;
    dev = inode->i_sb->s_bdev;
    if (dev && dev->ops == &loop_ops)
        return -ELOOP;

    readonly = (config->info.lo_flags & LO_FLAGS_READ_ONLY) ||
               !FILE_RDWR(file) || !FILE_RDWR(backing);
    if (!readonly && (!backing->f_ops->write || !backing->f_ops->fsync))
        return -EOPNOTSUPP;
    mutex_lock(&inode->mutex);
    size = inode->i_size;
    mutex_unlock(&inode->mutex);
    if (config->info.lo_offset >= size)
        return -EINVAL;
    length = size - config->info.lo_offset;
    if (config->info.lo_sizelimit && config->info.lo_sizelimit < length)
        length = config->info.lo_sizelimit;
    if (length < BLK_SECTOR_SIZE)
        return -EINVAL;

    ret = -EBUSY;
    mutex_lock(&loop_mutex);
    if (loop->backing)
        goto out;
    for (unsigned int i = 0; i < LOOP_SLOTS; i++)
        if (loops[i].backing && loops[i].backing->f_inode == inode)
            goto out;
    loop->info = (struct loop_info64){
        .lo_device = dev ? dev->dev : 0,
        .lo_inode = inode->i_ino,
        .lo_offset = config->info.lo_offset,
        .lo_sizelimit = config->info.lo_sizelimit,
        .lo_number = MINOR(loop->bdev.dev),
        .lo_flags = config->info.lo_flags | (readonly ? LO_FLAGS_READ_ONLY : 0),
    };
    memcpy(loop->info.lo_file_name, config->info.lo_file_name,
           sizeof(loop->info.lo_file_name) - 1);
    loop->bdev.sectors = length / BLK_SECTOR_SIZE;
    loop->backing = move(backing);
    ret = 0;
out:
    mutex_unlock(&loop_mutex);
    return ret;
}

static int loop_clear(struct loop_device *loop)
{
    struct file *backing = NULL;
    int ret = 0;

    mutex_lock(&loop_mutex);
    if (!loop->backing)
        ret = -ENXIO;
    else if (loop->claimed || loop->openers > 1)
        loop->info.lo_flags |= LO_FLAGS_AUTOCLEAR;
    else
        backing = loop_unbind(loop);
    mutex_unlock(&loop_mutex);
    fput(backing);
    return ret;
}

static long loop_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct loop_device *loop = to_loop(file->f_private);
    void *user = (void *)arg;

    switch (cmd) {
    case LOOP_CONFIGURE: {
        struct loop_config config;

        if (copy_from_user(&config, user, sizeof(config)))
            return -EFAULT;
        return loop_configure(loop, file, &config);
    }
    case LOOP_GET_STATUS64: {
        struct loop_info64 info;

        mutex_lock(&loop_mutex);
        if (!loop->backing) {
            mutex_unlock(&loop_mutex);
            return -ENXIO;
        }
        info = loop->info;
        mutex_unlock(&loop_mutex);
        return copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
    }
    case LOOP_CLR_FD:
        return loop_clear(loop);
    default:
        return -ENOTTY;
    }
}

static int loop_file_open(struct file *file)
{
    struct loop_device *loop = to_loop(file->f_private);

    mutex_lock(&loop_mutex);
    loop->openers++;
    mutex_unlock(&loop_mutex);
    return 0;
}

static int loop_file_release(struct file *file)
{
    struct loop_device *loop = to_loop(file->f_private);
    struct file *backing;

    mutex_lock(&loop_mutex);
    loop->openers--;
    backing = loop_autoclear(loop);
    mutex_unlock(&loop_mutex);
    return fput(backing);
}

static const struct file_operations loop_fops = {
    .open = loop_file_open,
    .ioctl = loop_ioctl,
    .release = loop_file_release,
};

static int loop_init(void)
{
    for (unsigned int i = 0; i < LOOP_SLOTS; i++) {
        struct blkdev *dev = &loops[i].bdev;

        dev->dev = MKDEV(LOOP_MAJOR, i);
        dev->ops = &loop_ops;
        dev->fops = &loop_fops;
        blkdev_register(dev);
    }
    return 0;
}

MODULE_INIT(loop_init, 86);

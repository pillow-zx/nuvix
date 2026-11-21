#include <nuvix/chardev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/module.h>

static loff_t mem_llseek(struct file *file, loff_t offset, int whence)
{
    (void)offset;
    (void)whence;
    mutex_lock(&file->f_pos_lock);
    file->f_pos = 0;
    mutex_unlock(&file->f_pos_lock);
    return 0;
}

static ssize_t null_read(struct kiocb *iocb, struct kiov *iov)
{
    (void)iocb;
    (void)iov;
    return 0;
}

static ssize_t null_write(struct kiocb *iocb, struct kiov *iov)
{
    size_t count = iov->count;

    (void)iocb;
    kiov_advance(iov, count);
    return count;
}

static ssize_t full_read(struct kiocb *iocb, struct kiov *iov)
{
    static const char zeros[256];
    size_t done = 0;

    (void)iocb;
    while (iov->count) {
        size_t count = MIN(iov->count, sizeof(zeros));
        size_t copied = copy_to_kiov(zeros, count, iov);

        done += copied;
        if (copied != count)
            return done ? (ssize_t)done : -EFAULT;
    }
    return done;
}

static ssize_t full_write(struct kiocb *iocb, struct kiov *iov)
{
    (void)iocb;
    (void)iov;
    return -ENOSPC;
}

static const struct file_operations null_fops = {
    .read = null_read,
    .write = null_write,
    .llseek = mem_llseek,
};

static const struct file_operations full_fops = {
    .read = full_read,
    .write = full_write,
    .llseek = mem_llseek,
};

static struct chardev null_dev = CHARDEV_INIT(MKDEV(1, 3), &null_fops, NULL);
static struct chardev full_dev = CHARDEV_INIT(MKDEV(1, 7), &full_fops, NULL);

static int mem_init(void)
{
    chardev_register(&null_dev);
    chardev_register(&full_dev);
    return 0;
}
MODULE_INIT(mem_init, 86);

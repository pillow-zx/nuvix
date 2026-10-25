#include <nuvix/vfs.h>
#include <nuvix/errno.h>
#include <nuvix/refcount.h>
#include <nuvix/slab.h>
#include <nuvix/fat.h>
#include <nuvix/mutex.h>
#include <nuvix/macro.h>
#include <nuvix/math.h>
#include <nuvix/syscall.h>
#include <nuvix/fdtable.h>
#include <nuvix/layout.h>
#include <nuvix/uaccess.h>
#include <asm/trap.h>

struct file *vfs_open(const char *path, unsigned long flags)
{
    struct file *file __free(kfree) = NULL;
    int ret;

    file = kzalloc(sizeof(*file));
    if (!file)
        return ERR_PTR(-ENOMEM);

    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_pos = 0;

    ret = fat_open(file, path, flags);
    if (ret)
        return ERR_PTR(ret);

    return move(file);
}

ssize_t vfs_read(struct file *file, void *buffer, size_t len, loff_t offset)
{
    if (!len)
        return 0;

    len = MIN(len, (size_t)SSIZE_MAX);

    return file->f_ops->read(file, buffer, len, &offset);
}

ssize_t vfs_write(struct file *file, const void *buffer, size_t len,
                  loff_t offset)
{
    if (!len)
        return 0;

    len = MIN(len, (size_t)SSIZE_MAX);

    return file->f_ops->write(file, buffer, len, &offset);
}

struct file *fget(struct file *file)
{
    if (file)
        refcount_inc(&file->f_count);
    return file;
}

int fput(struct file *file)
{
    int ret = 0;

    if (!file)
        return 0;
    if (!refcount_dec_and_test(&file->f_count))
        return 0;
    if (file->f_ops && file->f_ops->release)
        ret = file->f_ops->release(file);
    kfree(file);
    return ret;
}

long sys_openat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    unsigned long flags = syscallarg(frame, 2) & ~O_LARGEFILE;
    char *path __free(kfree) = NULL;
    struct file *file __free(fput) = NULL;
    size_t len;
    int fd;

    path = kmalloc(PATH_MAX);
    if (!path)
        return -ENOMEM;
    for (len = 0; len < PATH_MAX; ++len) {
        if (copy_from_user(path + len, upath + len, 1))
            return -EFAULT;
        if (!path[len])
            break;
    }
    if (len == PATH_MAX)
        return -ENAMETOOLONG;
    if (path[0] != '/' && dirfd != AT_FDCWD)
        return -EOPNOTSUPP;

    file = vfs_open(path, flags);
    if (IS_ERR(file))
        return PTR_ERR(move(file));
    fd = fd_alloc(current->fdtable);
    if (fd < 0)
        return fd;
    fd_install(current->fdtable, (unsigned long)fd, move(file));
    return fd;
}

long sys_close(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);

    return fd_close(current->fdtable, fd);
}

static ssize_t file_uread_locked(struct file *file, void *ubuf, size_t len,
                                 void *buf)
{
    size_t done = 0;
    ssize_t ret = 0;

    while (done < len) {
        size_t count = MIN(len - done, (size_t)PAGE_SIZE);
        size_t missing;
        size_t copied;

        ret = vfs_read(file, buf, count, file->f_pos);
        if (ret <= 0)
            break;

        missing = copy_to_user(ubuf + done, buf, (size_t)ret);
        copied = (size_t)ret - missing;
        file->f_pos += copied;
        done += copied;
        if (missing) {
            ret = -EFAULT;
            break;
        }
        if ((size_t)ret < count)
            break;
    }
    return done ? (ssize_t)done : ret;
}

static ssize_t file_uwrite_locked(struct file *file, const void *ubuf,
                                  size_t len, void *buf)
{
    size_t done = 0;
    ssize_t ret = 0;

    while (done < len) {
        size_t count = MIN(len - done, (size_t)PAGE_SIZE);
        size_t missing = copy_from_user(buf, ubuf + done, count);
        size_t copied = count - missing;

        if (!copied) {
            ret = -EFAULT;
            break;
        }

        ret = vfs_write(file, buf, copied, file->f_pos);
        if (ret <= 0)
            break;

        file->f_pos += ret;
        done += (size_t)ret;
        if ((size_t)ret < count)
            break;
    }
    return done ? (ssize_t)done : ret;
}

long sys_read(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    char *ubuf = (char *)syscallarg(frame, 1);
    size_t len = syscallarg(frame, 2);

    char *buf __free(kfree) = NULL;
    struct fdtable *fdt = current->fdtable;
    struct file *file __free(fput) = fd_open(fdt, fd);
    ssize_t ret;

    if (!file || !FILE_READ(file))
        return -EBADF;

    len = MIN(len, (size_t)SSIZE_MAX);
    if (!len)
        return 0;

    buf = kmalloc(MIN(len, (size_t)PAGE_SIZE));
    if (!buf)
        return -ENOMEM;

    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;
    ret = file_uread_locked(file, ubuf, len, buf);
    mutex_unlock(&file->f_pos_lock);

    return ret;
}

long sys_write(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    const char *ubuf = (const char *)syscallarg(frame, 1);
    size_t len = syscallarg(frame, 2);

    char *buf __free(kfree) = NULL;
    struct fdtable *fdt = current->fdtable;
    struct file *file __free(fput) = fd_open(fdt, fd);
    ssize_t ret;

    if (!file || !FILE_WRITE(file))
        return -EBADF;

    len = MIN(len, (size_t)SSIZE_MAX);
    if (!len)
        return 0;

    buf = kmalloc(MIN(len, (size_t)PAGE_SIZE));
    if (!buf)
        return -ENOMEM;

    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;
    ret = file_uwrite_locked(file, ubuf, len, buf);
    mutex_unlock(&file->f_pos_lock);

    return ret;
}

long sys_readv(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    const struct iovec *uiov = (const struct iovec *)syscallarg(frame, 1);
    int iovcnt = syscallarg(frame, 2);

    struct file *file __free(fput) = NULL;
    struct iovec *iov __free(kfree) = NULL;
    void *buf __free(kfree) = NULL;

    size_t total = 0;
    size_t done = 0;
    size_t bytes;
    ssize_t ret = 0;

    file = fd_open(current->fdtable, fd);
    if (!file)
        return -EBADF;

    if (!FILE_READ(file))
        return -EBADF;

    if (iovcnt < 0 || iovcnt > UIO_MAXIOV)
        return -EINVAL;

    if (!iovcnt)
        return 0;

    bytes = (size_t)iovcnt * sizeof(*iov);
    iov = kmalloc(bytes);
    if (!iov)
        return -ENOMEM;

    if (copy_from_user(iov, uiov, bytes))
        return -EFAULT;

    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > (size_t)SSIZE_MAX - total)
            return -EINVAL;
        total += iov[i].iov_len;
    }

    if (!total)
        return 0;

    buf = kmalloc(MIN(total, (size_t)PAGE_SIZE));
    if (!buf)
        return -ENOMEM;

    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;
    for (int i = 0; i < iovcnt; ++i) {
        size_t len = iov[i].iov_len;

        if (!len)
            continue;

        ret = file_uread_locked(file, iov[i].iov_base, len, buf);
        if (ret < 0)
            break;

        done += (size_t)ret;
        if ((size_t)ret < len)
            break;
    }
    mutex_unlock(&file->f_pos_lock);

    return done ? (ssize_t)done : ret;
}

long sys_writev(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    const struct iovec *uiov = (const struct iovec *)syscallarg(frame, 1);
    int iovcnt = syscallarg(frame, 2);

    struct file *file __free(fput) = NULL;
    struct iovec *iov __free(kfree) = NULL;
    void *buf __free(kfree) = NULL;

    size_t total = 0;
    size_t done = 0;
    size_t bytes;
    ssize_t ret = 0;

    file = fd_open(current->fdtable, fd);
    if (!file)
        return -EBADF;

    if (!FILE_WRITE(file))
        return -EBADF;

    if (iovcnt < 0 || iovcnt > UIO_MAXIOV)
        return -EINVAL;

    if (!iovcnt)
        return 0;

    bytes = (size_t)iovcnt * sizeof(*iov);
    iov = kmalloc(bytes);
    if (!iov)
        return -ENOMEM;

    if (copy_from_user(iov, uiov, bytes))
        return -EFAULT;

    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > (size_t)SSIZE_MAX - total)
            return -EINVAL;
        total += iov[i].iov_len;
    }

    if (!total)
        return 0;

    buf = kmalloc(MIN(total, (size_t)PAGE_SIZE));
    if (!buf)
        return -ENOMEM;

    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;
    for (int i = 0; i < iovcnt; ++i) {
        size_t len = iov[i].iov_len;

        if (!len)
            continue;

        ret = file_uwrite_locked(file, iov[i].iov_base, len, buf);
        if (ret < 0)
            break;

        done += (size_t)ret;
        if ((size_t)ret < len)
            break;
    }
    mutex_unlock(&file->f_pos_lock);

    return done ? (ssize_t)done : ret;
}

long sys_fchmodat(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

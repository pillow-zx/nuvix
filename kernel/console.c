#include <nuvix/console.h>
#include <nuvix/vfs.h>
#include <nuvix/fdtable.h>
#include <nuvix/ns16550a.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>

static ssize_t console_read(struct kiocb *iocb, struct kiov *iov)
{
    char *buffer __free(kfree) = NULL;
    size_t count = MIN(iov->count, (size_t)PAGE_SIZE);
    size_t copied;
    ssize_t ret;

    (void)iocb;
    buffer = kmalloc(count);
    if (!buffer)
        return -ENOMEM;
    ret = ns16550a_read(buffer, count);
    if (ret <= 0)
        return ret;
    copied = copy_to_kiov(buffer, (size_t)ret, iov);
    return copied ? (ssize_t)copied : -EFAULT;
}

static ssize_t console_write(struct kiocb *iocb, struct kiov *iov)
{
    char *buffer __free(kfree) = NULL;
    size_t done = 0;
    ssize_t ret = 0;

    (void)iocb;
    buffer = kmalloc(MIN(iov->count, (size_t)PAGE_SIZE));
    if (!buffer)
        return -ENOMEM;
    while (iov->count) {
        struct kiov next = *iov;
        size_t count = MIN(iov->count, (size_t)PAGE_SIZE);
        size_t copied = copy_from_kiov(buffer, count, &next);

        if (!copied) {
            ret = -EFAULT;
            break;
        }
        ret = ns16550a_write(buffer, copied);
        if (ret <= 0)
            break;
        kiov_advance(iov, (size_t)ret);
        done += (size_t)ret;
        if ((size_t)ret < copied)
            break;
        if (copied < count) {
            ret = -EFAULT;
            break;
        }
    }
    return done ? (ssize_t)done : ret;
}

static const struct file_operations console_fops = {
    .read = console_read,
    .write = console_write,
};

static struct file *console_open(unsigned int flags)
{
    struct file *file = kzalloc(sizeof(*file));

    if (!file)
        return NULL;
    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_ops = &console_fops;
    return file;
}

int console_init(struct fdtable *fdt)
{
    struct file *input __free(fput) = NULL;
    struct file *output __free(fput) = NULL;
    struct file *error __free(fput) = NULL;

    input = console_open(O_RDONLY);
    if (!input)
        return -ENOMEM;
    output = console_open(O_WRONLY);
    if (!output)
        return -ENOMEM;
    error = console_open(O_WRONLY);

    fd_install(fdt, stdin, move(input));
    fd_install(fdt, stdout, move(output));
    fd_install(fdt, stderr, move(error));
    return 0;
}

#include <nuvix/tty.h>
#include <nuvix/vfs.h>
#include <nuvix/fdtable.h>
#include <nuvix/ns16550a.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/task.h>

struct tty {
    struct wait_queue rwait;
    struct mutex rlock;
    unsigned int head;
    unsigned int tail;
    unsigned int canon;
    char buffer[PAGE_SIZE];
};

static struct tty gtty;

static void tty_receive(struct tty *tty, char c)
{
    unsigned long flags;
    unsigned int used;

    if (c == '\r')
        c = '\n';
    spin_lock_irqsave(&tty->rwait.lock, flags);
    used = tty->head - tty->tail;
    if (used == PAGE_SIZE || (used == PAGE_SIZE - 1 && c != '\n')) {
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        return;
    }
    tty->buffer[tty->head & (PAGE_SIZE - 1)] = c;
    ++tty->head;
    spin_unlock_irqrestore(&tty->rwait.lock, flags);

    if (c == '\n') {
        (void)ns16550a_write("\r\n", 2);
        spin_lock_irqsave(&tty->rwait.lock, flags);
        tty->canon = tty->head;
        wake_up_all_locked(&tty->rwait);
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
    } else {
        (void)ns16550a_write(&c, 1);
    }
}

static int tty_task(void *arg)
{
    struct tty *tty = arg;

    for (;;) {
        char c;
        ssize_t ret = ns16550a_read(&c, 1);

        Assert(ret == 1, "TTY receive failed (%ld)\n", ret);
        tty_receive(tty, c);
    }
}

static ssize_t tty_read(struct kiocb *iocb, struct kiov *iov)
{
    struct tty *tty = iocb->file->f_private;
    unsigned long flags;
    size_t done = 0;
    int ret;

    for (;;) {
        ret = wait_event_interruptible(&tty->rwait, tty->tail != tty->canon);
        if (ret)
            return -EINTR;
        ret = mutex_lock_interruptible(&tty->rlock);
        if (ret)
            return ret;
        spin_lock_irqsave(&tty->rwait.lock, flags);
        if (tty->tail != tty->canon)
            break;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        mutex_unlock(&tty->rlock);
    }

    while (iov->count) {
        unsigned int offset = tty->tail & (PAGE_SIZE - 1);
        size_t count =
            MIN((size_t)(tty->canon - tty->tail), (size_t)PAGE_SIZE - offset);
        char *buffer = tty->buffer + offset;
        char *newline;
        size_t copied;

        count = MIN(count, iov->count);
        newline = memchr(buffer, '\n', count);
        if (newline)
            count = (size_t)(newline - buffer) + 1;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        copied = copy_to_kiov(buffer, count, iov);
        spin_lock_irqsave(&tty->rwait.lock, flags);
        tty->tail += copied;
        done += copied;
        if (copied != count) {
            ret = -EFAULT;
            break;
        }
        if (newline)
            break;
    }
    spin_unlock_irqrestore(&tty->rwait.lock, flags);
    mutex_unlock(&tty->rlock);
    return done ? (ssize_t)done : ret;
}

static ssize_t tty_write(struct kiocb *iocb, struct kiov *iov)
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

static const struct file_operations tty_fops = {
    .read = tty_read,
    .write = tty_write,
};

static struct file *tty_open(unsigned int flags)
{
    struct file *file = kzalloc(sizeof(*file));

    if (!file)
        return NULL;
    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_ops = &tty_fops;
    file->f_private = &gtty;
    return file;
}

int tty_init(void)
{
    struct task *task __free(task_put) = task_alloc(false);

    if (!task)
        return -ENOMEM;
    wait_queue_init(&gtty.rwait);
    mutex_init(&gtty.rlock);
    arch_ktask_init(&task->ctx, tty_task, &gtty);
    sched_publish(task);
    return 0;
}

int init_stdio(struct fdtable *fdt)
{
    struct file *input __free(fput) = NULL;
    struct file *output __free(fput) = NULL;
    struct file *error __free(fput) = NULL;

    input = tty_open(O_RDONLY);
    if (!input)
        return -ENOMEM;
    output = tty_open(O_WRONLY);
    if (!output)
        return -ENOMEM;
    error = tty_open(O_WRONLY);
    if (!error)
        return -ENOMEM;

    fd_install(fdt, stdin, move(input));
    fd_install(fdt, stdout, move(output));
    fd_install(fdt, stderr, move(error));
    return 0;
}

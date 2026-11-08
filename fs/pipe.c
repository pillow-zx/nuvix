#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/wait.h>
#include <nuvix/fdtable.h>
#include <nuvix/task.h>
#include <nuvix/uaccess.h>
#include <nuvix/syscall.h>
#include <asm/trap.h>

#define PIPE_BUF_SIZE PAGE_SIZE

struct pipe {
    struct mutex mutex;
    struct wait_queue wq;
    struct inode *inode;
    char *buffer;
    unsigned int head;
    unsigned int tail;
    unsigned int readers;
    unsigned int writers;
};

static atomic_t pipe_ino = ATOMIC_INIT(1);

static void pipe_sigpipe(void)
{
    siginfo_t info = {0};

    info.si_signo = SIGPIPE;
    info.si_code = SI_USER;
    info.si_pid = task_pid_nr(current, PIDTYPE_TGID);
    send_signal(current, &info);
}

static ssize_t pipe_read(struct kiocb *iocb, struct kiov *iov)
{
    struct file *file = iocb->file;
    struct pipe *pipe = file->f_private;
    unsigned long flags;
    size_t done = 0;
    int ret = 0;

    if (!FILE_READ(file))
        return -EBADF;

    ret = mutex_lock_interruptible(&pipe->mutex);
    if (ret)
        return ret;

    for (;;) {
        spin_lock_irqsave(&pipe->wq.lock, flags);
        if (pipe->head != pipe->tail || !pipe->writers) {
            spin_unlock_irqrestore(&pipe->wq.lock, flags);
            break;
        }
        spin_unlock_irqrestore(&pipe->wq.lock, flags);
        if (file->f_flags & O_NONBLOCK) {
            mutex_unlock(&pipe->mutex);
            return -EAGAIN;
        }
        mutex_unlock(&pipe->mutex);
        ret = wait_event_interruptible(&pipe->wq, pipe->head != pipe->tail ||
                                                      !pipe->writers);
        if (ret)
            return -EINTR;
        ret = mutex_lock_interruptible(&pipe->mutex);
        if (ret)
            return ret;
    }

    while (iov->count && pipe->head != pipe->tail) {
        unsigned int offset = pipe->tail & (PIPE_BUF_SIZE - 1);
        size_t count = MIN((size_t)(pipe->head - pipe->tail),
                           (size_t)PIPE_BUF_SIZE - offset);
        size_t copied;

        count = MIN(count, iov->count);
        copied = copy_to_kiov(pipe->buffer + offset, count, iov);
        spin_lock_irqsave(&pipe->wq.lock, flags);
        pipe->tail += copied;
        spin_unlock_irqrestore(&pipe->wq.lock, flags);
        done += copied;
        if (copied != count) {
            ret = -EFAULT;
            break;
        }
    }
    mutex_unlock(&pipe->mutex);

    if (done)
        wake_up_all(&pipe->wq);

    return done ? (ssize_t)done : ret;
}

static ssize_t pipe_write(struct kiocb *iocb, struct kiov *iov)
{
    struct file *file = iocb->file;
    struct pipe *pipe = file->f_private;
    size_t total = iov->count;
    bool atomic = total <= PIPE_BUF_SIZE;
    unsigned long flags;
    size_t done = 0;
    int ret;

    if (!FILE_WRITE(file))
        return -EBADF;

    ret = mutex_lock_interruptible(&pipe->mutex);
    if (ret)
        return ret;

    while (iov->count) {
        unsigned int offset;
        size_t need;
        size_t count;
        size_t copied;

        spin_lock_irqsave(&pipe->wq.lock, flags);
        if (!pipe->readers) {
            spin_unlock_irqrestore(&pipe->wq.lock, flags);
            mutex_unlock(&pipe->mutex);
            pipe_sigpipe();
            return done ? (ssize_t)done : -EPIPE;
        }
        need = atomic ? iov->count : 1;
        if (PIPE_BUF_SIZE - (pipe->head - pipe->tail) < need) {
            spin_unlock_irqrestore(&pipe->wq.lock, flags);
            if (file->f_flags & O_NONBLOCK) {
                mutex_unlock(&pipe->mutex);
                return done ? (ssize_t)done : -EAGAIN;
            }
            mutex_unlock(&pipe->mutex);
            ret = wait_event_interruptible(
                &pipe->wq,
                !pipe->readers ||
                    PIPE_BUF_SIZE - (pipe->head - pipe->tail) >= need);
            if (ret)
                return done ? (ssize_t)done : -EINTR;
            ret = mutex_lock_interruptible(&pipe->mutex);
            if (ret)
                return ret;
            continue;
        }
        offset = pipe->head & (PIPE_BUF_SIZE - 1);
        count = MIN((size_t)(PIPE_BUF_SIZE - (pipe->head - pipe->tail)),
                    (size_t)PIPE_BUF_SIZE - offset);
        count = MIN(count, iov->count);
        spin_unlock_irqrestore(&pipe->wq.lock, flags);

        copied = copy_from_kiov(pipe->buffer + offset, count, iov);

        spin_lock_irqsave(&pipe->wq.lock, flags);
        pipe->head += copied;
        spin_unlock_irqrestore(&pipe->wq.lock, flags);
        done += copied;
        wake_up_all(&pipe->wq);
        if (copied != count) {
            mutex_unlock(&pipe->mutex);
            return done ? (ssize_t)done : -EFAULT;
        }
    }

    mutex_unlock(&pipe->mutex);
    return (ssize_t)done;
}

static int pipe_release(struct file *file)
{
    struct pipe *pipe = file->f_private;
    unsigned long flags;
    bool dead;

    spin_lock_irqsave(&pipe->wq.lock, flags);
    if (FILE_READ(file))
        --pipe->readers;
    else
        --pipe->writers;
    dead = !pipe->readers && !pipe->writers;
    if (!dead)
        wake_up_all_locked(&pipe->wq);
    spin_unlock_irqrestore(&pipe->wq.lock, flags);

    if (dead) {
        iput(pipe->inode);
        kfree(pipe->buffer);
        kfree(pipe);
    }
    return 0;
}

static poll_t pipe_poll(struct file *file, struct poll_table *table)
{
    struct pipe *pipe = file->f_private;
    unsigned long flags;
    poll_t mask = 0;

    spin_lock_irqsave(&pipe->wq.lock, flags);
    poll_add(&pipe->wq, table);
    if (FILE_READ(file)) {
        if (pipe->head != pipe->tail)
            mask |= POLLIN;
        if (!pipe->writers)
            mask |= POLLIN | POLLHUP;
    }
    if (FILE_WRITE(file)) {
        if (pipe->head - pipe->tail < PIPE_BUF_SIZE)
            mask |= POLLOUT;
        if (!pipe->readers)
            mask |= POLLERR;
    }
    spin_unlock_irqrestore(&pipe->wq.lock, flags);
    return mask;
}

static const struct file_operations pipe_fops = {
    .read = pipe_read,
    .write = pipe_write,
    .poll = pipe_poll,
    .release = pipe_release,
};

static int pipe_getattr(struct inode *inode, struct kstat *stat)
{
    mutex_lock(&inode->mutex);
    stat->ino = inode->i_ino;
    stat->mode = (inode->i_mode & S_IFMT) | 0777;
    stat->nlink = inode->i_nlink;
    stat->size = 0;
    stat->blksize = PIPE_BUF_SIZE;
    mutex_unlock(&inode->mutex);
    return 0;
}

static const struct inode_operations pipe_inode_ops = {
    .getattr = pipe_getattr,
};

static struct file *pipe_file(unsigned int flags, struct pipe *pipe)
{
    struct file *file = kzalloc(sizeof(*file));

    if (!file)
        return NULL;
    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_inode = pipe->inode;
    file->f_ops = pipe->inode->i_fops;
    file->f_private = pipe;
    return file;
}

static int pipe_alloc(struct file **rfile, struct file **wfile,
                      unsigned int flags)
{
    struct pipe *pipe = kzalloc(sizeof(*pipe));
    struct file *r;
    struct file *w;

    if (!pipe)
        return -ENOMEM;
    pipe->inode = i_alloc(NULL);
    pipe->buffer = kmalloc(PIPE_BUF_SIZE);
    pipe->inode->i_nlink = 1;
    if (!pipe->inode || !pipe->buffer)
        goto nomem;
    mutex_init(&pipe->mutex);
    wait_queue_init(&pipe->wq);
    pipe->inode->i_ino = (uint64_t)atomic_inc_return(&pipe_ino);
    pipe->inode->i_mode = S_IFIFO | 0600;
    pipe->inode->i_ops = &pipe_inode_ops;
    pipe->inode->i_fops = &pipe_fops;
    pipe->readers = 1;
    pipe->writers = 1;

    r = pipe_file(O_RDONLY | (flags & O_NONBLOCK), pipe);
    if (!r)
        goto nomem;
    w = pipe_file(O_WRONLY | (flags & O_NONBLOCK), pipe);
    if (!w) {
        kfree(r);
        goto nomem;
    }

    *rfile = r;
    *wfile = w;
    return 0;

nomem:
    iput(pipe->inode);
    kfree(pipe->buffer);
    kfree(pipe);
    return -ENOMEM;
}

long sys_pipe2(struct trap_frame *frame)
{
    int *upipefd = (int *)syscallarg(frame, 0);
    unsigned int flags = syscallarg(frame, 1);
    struct fdtable *fdt = current->fdtable;
    struct file *rfile __free(fput) = NULL;
    struct file *wfile __free(fput) = NULL;
    int fd[2];
    int ret;

    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;

    fd[0] = fd_alloc(fdt);
    if (fd[0] < 0)
        return fd[0];
    fd[1] = fd_alloc(fdt);
    if (fd[1] < 0) {
        fd_release(fdt, (unsigned long)fd[0]);
        return fd[1];
    }

    ret = pipe_alloc(&rfile, &wfile, flags);
    if (ret)
        goto fail;

    if (copy_to_user(upipefd, fd, sizeof(fd))) {
        ret = -EFAULT;
        goto fail;
    }

    fd_replace(fdt, (unsigned long)fd[0], move(rfile), flags & O_CLOEXEC);
    fd_replace(fdt, (unsigned long)fd[1], move(wfile), flags & O_CLOEXEC);
    return 0;

fail:
    fd_release(fdt, (unsigned long)fd[0]);
    fd_release(fdt, (unsigned long)fd[1]);
    return ret;
}

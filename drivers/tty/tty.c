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
#include <nuvix/uaccess.h>

#define TTY_WRITE_CHUNK (PAGE_SIZE / 2)

struct tty {
    struct wait_queue rwait;
    struct mutex rlock;
    unsigned int head;
    unsigned int tail;
    unsigned int canon;
    struct termios termios;
    struct winsize winsize;
    char buffer[PAGE_SIZE];
};

static struct tty gtty;

static size_t tty_opost(unsigned int oflag, char c, char *out)
{
    size_t len = 0;

    if ((oflag & OPOST) && (oflag & ONLCR) && c == '\n')
        out[len++] = '\r';
    out[len++] = c;
    return len;
}

static void tty_receive(struct tty *tty, char c)
{
    unsigned long flags;
    unsigned int used;
    char echo[3];
    size_t len = 0;
    bool wake = false;

    spin_lock_irqsave(&tty->rwait.lock, flags);

    if ((tty->termios.c_iflag & ICRNL) && c == '\r')
        c = '\n';

    if ((tty->termios.c_lflag & ICANON) &&
        c == (char)tty->termios.c_cc[VERASE]) {
        if (tty->head != tty->canon) {
            --tty->head;
            if (tty->termios.c_lflag & ECHO) {
                echo[0] = '\b';
                echo[1] = ' ';
                echo[2] = '\b';
                len = 3;
            }
        }
    } else {
        used = tty->head - tty->tail;
        if (used < PAGE_SIZE && (used < PAGE_SIZE - 1 || c == '\n')) {
            tty->buffer[tty->head & (PAGE_SIZE - 1)] = c;
            ++tty->head;
            if (tty->termios.c_lflag & ECHO) {
                if (c == '\n') {
                    len = tty_opost(tty->termios.c_oflag, c, echo);
                } else {
                    echo[0] = c;
                    len = 1;
                }
            }
            if (!(tty->termios.c_lflag & ICANON) || c == '\n') {
                tty->canon = tty->head;
                wake = true;
            }
        }
    }
    if (wake)
        wake_up_all_locked(&tty->rwait);
    spin_unlock_irqrestore(&tty->rwait.lock, flags);

    if (len)
        (void)ns16550a_write(echo, len);
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
        if (!count)
            break;
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
    struct tty *tty = iocb->file->f_private;
    char *in __free(kfree) = NULL;
    char *out __free(kfree) = NULL;
    unsigned long flags;
    unsigned int oflag;
    size_t done = 0;
    ssize_t ret = 0;

    in = kmalloc(TTY_WRITE_CHUNK);
    out = kmalloc(2 * TTY_WRITE_CHUNK);
    if (!in || !out)
        return -ENOMEM;

    spin_lock_irqsave(&tty->rwait.lock, flags);
    oflag = tty->termios.c_oflag;
    spin_unlock_irqrestore(&tty->rwait.lock, flags);

    while (iov->count) {
        struct kiov next = *iov;
        size_t count = MIN(iov->count, (size_t)TTY_WRITE_CHUNK);
        size_t copied = copy_from_kiov(in, count, &next);
        size_t consumed = 0;
        size_t sent = 0;
        size_t len = 0;
        ssize_t written;

        if (copied != count) {
            ret = -EFAULT;
            break;
        }

        for (size_t i = 0; i < copied; ++i)
            len += tty_opost(oflag, in[i], out + len);

        written = ns16550a_write(out, len);
        if (written < 0) {
            ret = written;
            break;
        }

        for (size_t i = 0; i < copied; ++i) {
            char expand[2];
            size_t expanded = tty_opost(oflag, in[i], expand);

            if (sent + expanded > (size_t)written)
                break;
            sent += expanded;
            ++consumed;
        }

        kiov_advance(iov, consumed);
        done += consumed;

        if (consumed != copied) {
            ret = -EINTR;
            break;
        }
    }
    return done ? (ssize_t)done : ret;
}

static long tty_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct tty *tty = file->f_private;
    struct termios termios;
    struct winsize winsize;
    unsigned long flags;

    switch (cmd) {
    case TCGETS:
        spin_lock_irqsave(&tty->rwait.lock, flags);
        termios = tty->termios;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        if (copy_to_user((void *)arg, &termios, sizeof(termios)))
            return -EFAULT;
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (copy_from_user(&termios, (void *)arg, sizeof(termios)))
            return -EFAULT;
        spin_lock_irqsave(&tty->rwait.lock, flags);
        tty->termios = termios;
        if (!(termios.c_lflag & ICANON)) {
            tty->canon = tty->head;
            wake_up_all_locked(&tty->rwait);
        }
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        return 0;
    case TIOCGWINSZ:
        spin_lock_irqsave(&tty->rwait.lock, flags);
        winsize = tty->winsize;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        if (copy_to_user((void *)arg, &winsize, sizeof(winsize)))
            return -EFAULT;
        return 0;
    case TIOCSWINSZ:
        if (copy_from_user(&winsize, (void *)arg, sizeof(winsize)))
            return -EFAULT;
        spin_lock_irqsave(&tty->rwait.lock, flags);
        tty->winsize = winsize;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        return 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations tty_fops = {
    .read = tty_read,
    .write = tty_write,
    .ioctl = tty_ioctl,
};

static struct inode *tty_inode;

static int tty_inode_getattr(struct inode *inode, struct kstat *stat)
{
    stat->ino = inode->i_ino;
    stat->mode = (inode->i_mode & S_IFMT) | 0777;
    stat->nlink = 1;
    stat->blksize = PAGE_SIZE;
    return 0;
}

static const struct inode_operations tty_inode_ops = {
    .getattr = tty_inode_getattr,
};

static struct file *tty_open(unsigned int flags)
{
    struct file *file = kzalloc(sizeof(*file));

    if (!file)
        return NULL;
    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_inode = tty_inode;
    file->f_ops = tty_inode->i_fops;
    file->f_private = tty_inode->i_private;
    return file;
}

int tty_init(void)
{
    struct task *task __free(task_put) = task_alloc(false);

    if (!task)
        return -ENOMEM;
    wait_queue_init(&gtty.rwait);
    mutex_init(&gtty.rlock);
    gtty.termios = (struct termios){
        .c_iflag = ICRNL | IXON,
        .c_oflag = OPOST | ONLCR,
        .c_cflag = CS8 | CREAD,
        .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
        .c_cc = {[VINTR] = 3,
                 [VQUIT] = 28,
                 [VERASE] = 127,
                 [VKILL] = 21,
                 [VEOF] = 4,
                 [VMIN] = 1,
                 [VSTART] = 17,
                 [VSTOP] = 19,
                 [VSUSP] = 26,
                 [VREPRINT] = 18,
                 [VDISCARD] = 15,
                 [VWERASE] = 23,
                 [VLNEXT] = 22},
    };
    gtty.winsize = (struct winsize){.ws_row = 24, .ws_col = 80};

    tty_inode = i_alloc();
    if (!tty_inode)
        return -ENOMEM;
    tty_inode->i_ino = 1;
    tty_inode->i_mode = S_IFCHR | 0600;
    tty_inode->i_ops = &tty_inode_ops;
    tty_inode->i_fops = &tty_fops;
    tty_inode->i_private = &gtty;

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

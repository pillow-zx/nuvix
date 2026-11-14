#include "uart.h"

#include <nuvix/tty.h>
#include <nuvix/chardev.h>
#include <nuvix/module.h>
#include <nuvix/vfs.h>
#include <nuvix/fdtable.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <nuvix/pid.h>
#include <nuvix/signal.h>
#include <nuvix/uaccess.h>

#define TTY_WRITE_CHUNK (PAGE_SIZE / 2)
#define TTY_RX_BATCH    64

struct tty {
    struct wait_queue rwait;
    struct mutex rlock;
    unsigned int head;
    unsigned int tail;
    unsigned int canon;
    struct termios2 termios;
    struct winsize winsize;
    struct pid *pgrp;
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

static void tty_receive(struct tty *tty, const char *data, size_t count)
{
    char echo[3 * TTY_RX_BATCH];
    unsigned long flags;
    unsigned int used;
    size_t len = 0;
    bool wake = false;

    Assert(count <= TTY_RX_BATCH, "TTY receive batch too large\n");
    spin_lock_irqsave(&tty->rwait.lock, flags);

    for (size_t i = 0; i < count; ++i) {
        char c = data[i];

        if ((tty->termios.c_lflag & ISIG) && tty->termios.c_cc[VINTR] &&
            (unsigned char)c == tty->termios.c_cc[VINTR]) {
            struct pid *pgrp = tty->pgrp;
            siginfo_t info = {.si_signo = SIGINT, .si_code = SI_KERNEL};

            if (pgrp)
                refcount_inc(&pgrp->refcount);
            spin_unlock_irqrestore(&tty->rwait.lock, flags);
            if (pgrp) {
                (void)send_group_signal(pgrp->nr, &info);
                pid_put(pgrp);
            }
            spin_lock_irqsave(&tty->rwait.lock, flags);
            continue;
        }

        if ((tty->termios.c_iflag & ICRNL) && c == '\r')
            c = '\n';

        if ((tty->termios.c_lflag & ICANON) &&
            c == (char)tty->termios.c_cc[VERASE]) {
            if (tty->head != tty->canon) {
                --tty->head;
                if (tty->termios.c_lflag & ECHO) {
                    echo[len++] = '\b';
                    echo[len++] = ' ';
                    echo[len++] = '\b';
                }
            }
            continue;
        }

        used = tty->head - tty->tail;
        if (used >= PAGE_SIZE || (used >= PAGE_SIZE - 1 && c != '\n'))
            continue;

        tty->buffer[tty->head & (PAGE_SIZE - 1)] = c;
        ++tty->head;
        if (tty->termios.c_lflag & ECHO)
            len += tty_opost(tty->termios.c_oflag, c, echo + len);

        if (!(tty->termios.c_lflag & ICANON) || c == '\n') {
            tty->canon = tty->head;
            wake = true;
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
    char buffer[TTY_RX_BATCH];

    for (;;) {
        ssize_t ret = ns16550a_read(buffer, sizeof(buffer));

        Assert(ret > 0, "TTY receive failed (%ld)\n", ret);
        tty_receive(tty, buffer, (size_t)ret);
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

struct tty_baud {
    unsigned int code;
    unsigned int baud;
};

static const struct tty_baud tty_bauds[] = {
    {B50, 50},         {B75, 75},         {B110, 110},     {B134, 134},
    {B150, 150},       {B200, 200},       {B300, 300},     {B600, 600},
    {B1200, 1200},     {B1800, 1800},     {B2400, 2400},   {B4800, 4800},
    {B9600, 9600},     {B19200, 19200},   {B38400, 38400}, {B57600, 57600},
    {B115200, 115200}, {B230400, 230400},
};

#define TTY_BAUDS_COUNT (sizeof(tty_bauds) / sizeof(tty_bauds[0]))

static const struct tty_baud *tty_baud_by_code(unsigned int code)
{
    for (size_t i = 0; i < TTY_BAUDS_COUNT; ++i)
        if (tty_bauds[i].code == code)
            return &tty_bauds[i];
    return NULL;
}

static const struct tty_baud *tty_baud_by_rate(unsigned int baud)
{
    for (size_t i = 0; i < TTY_BAUDS_COUNT; ++i)
        if (tty_bauds[i].baud == baud)
            return &tty_bauds[i];
    return NULL;
}

static void tty_termios_to_user(const struct termios2 *termios,
                                struct termios *ktermios)
{
    *ktermios = (struct termios){
        .c_iflag = termios->c_iflag,
        .c_oflag = termios->c_oflag,
        .c_cflag = termios->c_cflag,
        .c_lflag = termios->c_lflag,
        .c_line = termios->c_line,
    };
    memcpy(ktermios->c_cc, termios->c_cc, NCCS);
}

static int tty_termios_from_user(const struct termios *ktermios,
                                 struct termios2 *termios)
{
    unsigned int code = ktermios->c_cflag & CBAUD;
    const struct tty_baud *baud = tty_baud_by_code(code);

    termios->c_iflag = ktermios->c_iflag;
    termios->c_oflag = ktermios->c_oflag;
    termios->c_cflag = ktermios->c_cflag;
    termios->c_lflag = ktermios->c_lflag;
    termios->c_line = ktermios->c_line;
    memcpy(termios->c_cc, ktermios->c_cc, NCCS);
    if (code == BOTHER)
        return 0;
    if (!baud)
        return -EINVAL;
    termios->c_ispeed = baud->baud;
    termios->c_ospeed = baud->baud;
    return 0;
}

static int tty_termios_normalize(struct termios2 *termios)
{
    const struct tty_baud *baud = tty_baud_by_rate(termios->c_ospeed);

    if (!termios->c_ospeed)
        return -EINVAL;
    termios->c_ispeed = termios->c_ospeed;
    termios->c_cflag =
        (termios->c_cflag & ~CBAUD) | (baud ? baud->code : BOTHER);
    return 0;
}

static int tty_set_termios(struct tty *tty, struct termios2 *termios,
                           bool drain, bool discard)
{
    unsigned long flags;
    int ret;

    if (drain) {
        ret = ns16550a_flush_output();
        if (ret)
            return ret;
    }
    ret = ns16550a_set_termios(termios);
    if (ret)
        return ret;
    if (discard) {
        ns16550a_flush_input();
        mutex_lock(&tty->rlock);
    }
    spin_lock_irqsave(&tty->rwait.lock, flags);
    tty->termios = *termios;
    if (discard)
        tty->head = tty->tail = tty->canon = 0;
    if (!(termios->c_lflag & ICANON)) {
        tty->canon = tty->head;
        wake_up_all_locked(&tty->rwait);
    }
    spin_unlock_irqrestore(&tty->rwait.lock, flags);
    if (discard)
        mutex_unlock(&tty->rlock);
    return 0;
}

static long tty_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct tty *tty = file->f_private;
    struct termios2 termios;
    struct termios ktermios;
    struct winsize winsize;
    struct pid *pgrp __free(pid_put) = NULL;
    struct pid *old_pgrp __free(pid_put) = NULL;
    unsigned long flags;
    pid_t pgid;
    int ret;

    switch (cmd) {
    case TIOCGPGRP:
        pgrp = pid_get_group(0);
        spin_lock_irqsave(&tty->rwait.lock, flags);
        if (!tty->pgrp)
            tty->pgrp = move(pgrp);
        pgid = tty->pgrp->nr;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        if (copy_to_user((void *)arg, &pgid, sizeof(pgid)))
            return -EFAULT;
        return 0;
    case TIOCSPGRP:
        if (copy_from_user(&pgid, (void *)arg, sizeof(pgid)))
            return -EFAULT;
        if (pgid < 0)
            return -EINVAL;
        if (!pgid)
            return -ESRCH;
        pgrp = pid_get_group(pgid);
        if (!pgrp)
            return -ESRCH;
        spin_lock_irqsave(&tty->rwait.lock, flags);
        old_pgrp = tty->pgrp;
        tty->pgrp = move(pgrp);
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        return 0;
    case TCGETS:
        spin_lock_irqsave(&tty->rwait.lock, flags);
        tty_termios_to_user(&tty->termios, &ktermios);
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        if (copy_to_user((void *)arg, &ktermios, sizeof(ktermios)))
            return -EFAULT;
        return 0;
    case TCGETS2:
        spin_lock_irqsave(&tty->rwait.lock, flags);
        termios = tty->termios;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        if (copy_to_user((void *)arg, &termios, sizeof(termios)))
            return -EFAULT;
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (copy_from_user(&ktermios, (void *)arg, sizeof(ktermios)))
            return -EFAULT;
        spin_lock_irqsave(&tty->rwait.lock, flags);
        termios = tty->termios;
        spin_unlock_irqrestore(&tty->rwait.lock, flags);
        ret = tty_termios_from_user(&ktermios, &termios);
        if (ret)
            return ret;
        return tty_set_termios(tty, &termios, cmd != TCSETS, cmd == TCSETSF);
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
        if (copy_from_user(&termios, (void *)arg, sizeof(termios)))
            return -EFAULT;
        ret = tty_termios_normalize(&termios);
        if (ret)
            return ret;
        return tty_set_termios(tty, &termios, cmd != TCSETS2, cmd == TCSETSF2);
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

static poll_t tty_poll(struct file *file, struct poll_table *table)
{
    struct tty *tty = file->f_private;
    unsigned long flags;
    poll_t mask = 0;

    spin_lock_irqsave(&tty->rwait.lock, flags);
    if (FILE_READ(file))
        tty->tail != tty->canon ? mask |= POLLIN : poll_add(&tty->rwait, table);
    if (FILE_WRITE(file))
        mask |= POLLOUT;
    spin_unlock_irqrestore(&tty->rwait.lock, flags);
    return mask;
}

static const struct file_operations tty_fops = {
    .read = tty_read,
    .write = tty_write,
    .poll = tty_poll,
    .ioctl = tty_ioctl,
};

static struct chardev tty_devices[] = {
    {.dev = MKDEV(5, 0), .fops = &tty_fops, .private = &gtty},
    {.dev = MKDEV(5, 1), .fops = &tty_fops, .private = &gtty},
};

static int tty_register(void)
{
    for (int i = 0; i < ARRLEN(tty_devices); ++i)
        chardev_register(&tty_devices[i]);
    return 0;
}

MODULE_INIT(tty_register, 86);

int tty_init(void)
{
    struct task *task __free(task_put) = NULL;
    int ret;

    wait_queue_init(&gtty.rwait);
    mutex_init(&gtty.rlock);
    gtty.termios = (struct termios2){
        .c_iflag = ICRNL | IXON,
        .c_oflag = OPOST | ONLCR,
        .c_cflag = B115200 | CS8 | CREAD,
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
        .c_ispeed = 115200,
        .c_ospeed = 115200,
    };
    gtty.winsize = (struct winsize){.ws_row = 24, .ws_col = 80};
    ret = ns16550a_set_termios(&gtty.termios);
    if (ret)
        return ret;

    task = task_alloc(false);
    if (!task)
        return -ENOMEM;
    arch_ktask_init(&task->ctx, tty_task, &gtty);
    sched_publish(task);
    return 0;
}

int init_stdio(struct fdtable *fdt)
{
    struct file *input __free(fput) = NULL;
    struct file *output __free(fput) = NULL;
    struct file *error __free(fput) = NULL;

    input = vfs_open(AT_FDCWD, "/dev/console", O_RDONLY);
    if (IS_ERR(input))
        return PTR_ERR(move(input));
    output = vfs_open(AT_FDCWD, "/dev/console", O_WRONLY);
    if (IS_ERR(output))
        return PTR_ERR(move(output));
    error = vfs_open(AT_FDCWD, "/dev/console", O_WRONLY);
    if (IS_ERR(error))
        return PTR_ERR(move(error));

    fd_install(fdt, stdin, move(input));
    fd_install(fdt, stdout, move(output));
    fd_install(fdt, stderr, move(error));
    return 0;
}

#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

#ifdef CONFIG_TRACE_SYSRING
#include <nuvix/math.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/syscall.h>
#include <nuvix/uaccess.h>
#include <asm/trap.h>

#define SYSRING_SIZE              16384U

#define SYSLOG_ACTION_READ_ALL    3
#define SYSLOG_ACTION_SIZE_BUFFER 10

static struct {
    spinlock_t lock;
    unsigned int next;
    unsigned int used;
    char buffer[SYSRING_SIZE];
} sysring = {
    .lock = SPINLOCK_INITIALIZER,
};

static void sysring_write(const char *buf, unsigned int len)
{
    unsigned long flags;

    spin_lock_irqsave(&sysring.lock, flags);
    for (unsigned int i = 0; i < len; ++i) {
        sysring.buffer[sysring.next] = buf[i];
        sysring.next = (sysring.next + 1) % SYSRING_SIZE;
    }
    sysring.used = MIN(sysring.used + len, SYSRING_SIZE);
    spin_unlock_irqrestore(&sysring.lock, flags);
}

long sys_syslog(struct trap_frame *frame)
{
    int type = (int)syscallarg(frame, 0);
    char *buf = (char *)syscallarg(frame, 1);
    int len = (int)syscallarg(frame, 2);
    char *snapshot __free(kfree) = NULL;
    unsigned long flags;
    unsigned int count;
    unsigned int start;

    switch (type) {
    case SYSLOG_ACTION_SIZE_BUFFER:
        return SYSRING_SIZE;
    case SYSLOG_ACTION_READ_ALL:
        break;
    default:
        return -EINVAL;
    }

    if (!buf || len < 0)
        return -EINVAL;
    if (!len)
        return 0;
    if (!access_ok(buf, (size_t)len))
        return -EFAULT;

    count = MIN((unsigned int)len, SYSRING_SIZE);
    snapshot = kmalloc(count);
    if (!snapshot)
        return -ENOMEM;

    spin_lock_irqsave(&sysring.lock, flags);
    count = MIN(count, sysring.used);
    start = (sysring.next + SYSRING_SIZE - count) % SYSRING_SIZE;
    for (unsigned int i = 0; i < count; ++i)
        snapshot[i] = sysring.buffer[(start + i) % SYSRING_SIZE];
    spin_unlock_irqrestore(&sysring.lock, flags);

    if (copy_to_user(buf, snapshot, count))
        return -EFAULT;
    return count;
}
#endif

int vprintk(const char *fmt, va_list args)
{
    char buf[1024];
    va_list num;
    int len;

    va_copy(num, args);
    len = vsprintf(NULL, fmt, num);
    va_end(num);
    if (len >= (int)sizeof(buf))
        len = -EOVERFLOW;
    if (len >= 0)
        len = vsprintf(buf, fmt, args);

    IFDEF(CONFIG_TRACE_SYSRING, if (len > 0) sysring_write(buf, len));

    for (int i = 0; i < len; ++i) {
        if (buf[i] == '\n')
            putchar('\r');
        putchar((unsigned char)buf[i]);
    }
    return len;
}

int printk(const char *fmt, ...)
{
    va_list args;
    int len;

    va_start(args, fmt);
    len = vprintk(fmt, args);
    va_end(args);
    return len;
}

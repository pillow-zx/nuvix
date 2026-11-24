#include <nuvix/fdtable.h>
#include <nuvix/slab.h>
#include <nuvix/bitmap.h>
#include <nuvix/spinlock.h>
#include <nuvix/vfs.h>
#include <nuvix/errno.h>
#include <nuvix/assert.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <asm/trap.h>

struct fdtable *fdtable_alloc(void)
{
    struct fdtable *fdt __free(kfree) = kzalloc(sizeof(*fdt));

    if (!fdt)
        return NULL;
    fdt->files = kzalloc(BITS_PER_LONG * sizeof(*fdt->files));
    if (!fdt->files)
        return NULL;
    refcount_set(&fdt->refs, 1);
    spin_lock_init(&fdt->lock);
    return move(fdt);
}

void fdtable_get(struct fdtable *fdt)
{
    refcount_inc(&fdt->refs);
}

void fdtable_put(struct fdtable *fdt)
{
    unsigned long fd;

    if (!fdt || !refcount_dec_and_test(&fdt->refs))
        return;
    for_each_set_bit (fd, &fdt->bitmap, BITS_PER_LONG)
        fput(fdt->files[fd]);
    kfree(fdt->files);
    kfree(fdt);
}

int fd_alloc_from(struct fdtable *fdt, unsigned long min)
{
    unsigned long flags;
    unsigned long fd;

    if (min >= BITS_PER_LONG)
        return -EMFILE;

    spin_lock_irqsave(&fdt->lock, flags);

    fd = find_next_zero_bit(&fdt->bitmap, BITS_PER_LONG, min);
    if (fd < BITS_PER_LONG)
        set_bit(fd, &fdt->bitmap);

    spin_unlock_irqrestore(&fdt->lock, flags);

    return fd == BITS_PER_LONG ? -EMFILE : (int)fd;
}

int fd_alloc(struct fdtable *fdt)
{
    return fd_alloc_from(fdt, 0);
}

void fd_release(struct fdtable *fdt, unsigned long fd)
{
    unsigned long flags;

    spin_lock_irqsave(&fdt->lock, flags);
    Assert(fd < BITS_PER_LONG && test_bit(fd, &fdt->bitmap) && !fdt->files[fd],
           "fd release requires a reserved slot\n");
    clear_bit(fd, &fdt->bitmap);
    clear_bit(fd, &fdt->cloexec);
    spin_unlock_irqrestore(&fdt->lock, flags);
}

static void fd_set_cloexec_locked(struct fdtable *fdt, unsigned long fd,
                                  bool cloexec)
{
    if (cloexec)
        set_bit(fd, &fdt->cloexec);
    else
        clear_bit(fd, &fdt->cloexec);
}

void fd_install(struct fdtable *fdt, unsigned long fd, struct file *file,
                bool cloexec)
{
    unsigned long flags;

    spin_lock_irqsave(&fdt->lock, flags);
    Assert(file && !IS_ERR(file) && fd < BITS_PER_LONG &&
               test_bit(fd, &fdt->bitmap) && !fdt->files[fd],
           "fd installation requires a reserved slot and an opened file\n");
    fdt->files[fd] = file;
    fd_set_cloexec_locked(fdt, fd, cloexec);
    spin_unlock_irqrestore(&fdt->lock, flags);
}

struct file *fd_open(struct fdtable *fdt, unsigned long fd)
{
    struct file *file = NULL;
    unsigned long flags;

    spin_lock_irqsave(&fdt->lock, flags);
    if (fd < BITS_PER_LONG)
        file = fget(fdt->files[fd]);
    spin_unlock_irqrestore(&fdt->lock, flags);

    return file;
}

int fd_close(struct fdtable *fdt, unsigned long fd)
{
    struct file *file = NULL;
    unsigned long flags;

    spin_lock_irqsave(&fdt->lock, flags);
    if (fd < BITS_PER_LONG) {
        file = fdt->files[fd];
        if (file) {
            fdt->files[fd] = NULL;
            clear_bit(fd, &fdt->bitmap);
            clear_bit(fd, &fdt->cloexec);
        }
    }
    spin_unlock_irqrestore(&fdt->lock, flags);
    if (!file)
        return -EBADF;
    return fput(file);
}

struct fdtable *fdtable_clone(struct fdtable *src)
{
    struct fdtable *dst = fdtable_alloc();
    unsigned long fd, flags;

    if (!dst)
        return NULL;
    spin_lock_irqsave(&src->lock, flags);
    for_each_set_bit (fd, &src->bitmap, BITS_PER_LONG) {
        struct file *file = src->files[fd];

        if (!file)
            continue;
        dst->files[fd] = fget(file);
        set_bit(fd, &dst->bitmap);
    }
    dst->cloexec = src->cloexec & dst->bitmap;
    spin_unlock_irqrestore(&src->lock, flags);
    return dst;
}

void fdtable_cloexec(struct fdtable *fdt)
{
    unsigned long fd;

    for_each_set_bit (fd, &fdt->cloexec, BITS_PER_LONG)
        fd_close(fdt, fd);
}

void fd_set_cloexec(struct fdtable *fdt, unsigned long fd, bool enable)
{
    unsigned long flags;

    Assert(fd < BITS_PER_LONG, "fd cloexec update requires a valid fd\n");

    spin_lock_irqsave(&fdt->lock, flags);
    fd_set_cloexec_locked(fdt, fd, enable);
    spin_unlock_irqrestore(&fdt->lock, flags);
}

bool fd_test_cloexec(struct fdtable *fdt, unsigned long fd)
{
    unsigned long flags;
    bool set;

    Assert(fd < BITS_PER_LONG, "fd cloexec lookup requires a valid fd\n");

    spin_lock_irqsave(&fdt->lock, flags);
    set = test_bit(fd, &fdt->cloexec);
    spin_unlock_irqrestore(&fdt->lock, flags);

    return set;
}

long sys_dup(struct trap_frame *frame)
{
    struct fdtable *fdt = current->fdtable;
    struct file *file __free(fput) = NULL;
    int newfd;

    file = fd_open(fdt, syscallarg(frame, 0));
    if (!file)
        return -EBADF;

    newfd = fd_alloc(fdt);
    if (newfd < 0)
        return newfd;

    fd_install(fdt, (unsigned long)newfd, move(file), false);
    return newfd;
}

long sys_dup3(struct trap_frame *frame)
{
    unsigned int oldfd = syscallarg(frame, 0);
    unsigned int newfd = syscallarg(frame, 1);
    unsigned int flags = syscallarg(frame, 2);
    struct fdtable *fdt = current->fdtable;
    struct file *file, *replaced = NULL;
    unsigned long irqflags;
    long ret;

    if (flags & ~O_CLOEXEC)
        return -EINVAL;
    if (oldfd == newfd)
        return -EINVAL;
    if (oldfd >= BITS_PER_LONG || newfd >= BITS_PER_LONG)
        return -EBADF;

    spin_lock_irqsave(&fdt->lock, irqflags);
    file = fdt->files[oldfd];
    if (!file) {
        ret = -EBADF;
        goto out;
    }
    if (!fdt->files[newfd] && test_bit(newfd, &fdt->bitmap)) {
        ret = -EBUSY;
        goto out;
    }
    replaced = fdt->files[newfd];
    fdt->files[newfd] = fget(file);
    set_bit(newfd, &fdt->bitmap);
    fd_set_cloexec_locked(fdt, newfd, flags & O_CLOEXEC);
    ret = newfd;
out:
    spin_unlock_irqrestore(&fdt->lock, irqflags);
    fput(replaced);
    return ret;
}

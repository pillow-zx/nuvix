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

int fdtable_init(struct fdtable *fdt)
{
    struct file **files __free(kfree) = NULL;

    files = kzalloc(BITS_PER_LONG * sizeof(*files));
    if (!files)
        return -ENOMEM;

    spin_lock_init(&fdt->lock);
    bitmap_zero(&fdt->bitmap, BITS_PER_LONG);
    bitmap_zero(&fdt->cloexec, BITS_PER_LONG);
    fdt->files = move(files);
    set_bit(stdin, &fdt->bitmap);
    set_bit(stdout, &fdt->bitmap);
    set_bit(stderr, &fdt->bitmap);
    return 0;
}

void fdtable_destroy(struct fdtable *fdt)
{
    unsigned long fd;
    for_each_set_bit (fd, &fdt->bitmap, BITS_PER_LONG) {
        struct file *file = fdt->files[fd];
        if (file)
            fput(file);
    }
    kfree(fdt->files);
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

void fd_install(struct fdtable *fdt, unsigned long fd, struct file *file)
{
    unsigned long flags;

    spin_lock_irqsave(&fdt->lock, flags);
    Assert(file && !IS_ERR(file) && fd < BITS_PER_LONG &&
               test_bit(fd, &fdt->bitmap),
           "fd installation requires a reserved slot and an opened file\n");
    fdt->files[fd] = file;
    spin_unlock_irqrestore(&fdt->lock, flags);
}

struct file *fd_replace(struct fdtable *fdt, unsigned long fd,
                        struct file *file, bool cloexec)
{
    struct file *replaced;
    unsigned long flags;

    Assert(file && !IS_ERR(file) && fd < BITS_PER_LONG,
           "fd installation requires an opened file and a valid fd\n");

    spin_lock_irqsave(&fdt->lock, flags);
    replaced = fdt->files[fd];
    fdt->files[fd] = file;
    set_bit(fd, &fdt->bitmap);
    if (cloexec)
        set_bit(fd, &fdt->cloexec);
    else
        clear_bit(fd, &fdt->cloexec);
    spin_unlock_irqrestore(&fdt->lock, flags);

    return replaced;
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

void fdtable_clone(struct fdtable *dst, struct fdtable *src)
{
    unsigned long flags;

    spin_lock_irqsave(&src->lock, flags);
    dst->bitmap = src->bitmap;
    dst->cloexec = src->cloexec;
    for (unsigned long fd = 0; fd < BITS_PER_LONG; ++fd)
        dst->files[fd] = fget(src->files[fd]);
    spin_unlock_irqrestore(&src->lock, flags);
}

void fdtable_close_on_exec(struct fdtable *fdt)
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
    if (enable)
        set_bit(fd, &fdt->cloexec);
    else
        clear_bit(fd, &fdt->cloexec);
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

    fd_install(fdt, (unsigned long)newfd, move(file));
    return newfd;
}

long sys_dup3(struct trap_frame *frame)
{
    unsigned int oldfd = syscallarg(frame, 0);
    unsigned int newfd = syscallarg(frame, 1);
    unsigned int flags = syscallarg(frame, 2);
    struct fdtable *fdt = current->fdtable;
    struct file *file __free(fput) = NULL;
    struct file *replaced;

    if (flags & ~O_CLOEXEC)
        return -EINVAL;
    if (oldfd == newfd)
        return -EINVAL;
    if (newfd >= BITS_PER_LONG)
        return -EBADF;

    file = fd_open(fdt, oldfd);
    if (!file)
        return -EBADF;

    replaced = fd_replace(fdt, newfd, move(file), flags & O_CLOEXEC);
    fput(replaced);

    return newfd;
}

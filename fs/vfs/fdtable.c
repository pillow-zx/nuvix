#include <nuvix/fdtable.h>
#include <nuvix/slab.h>
#include <nuvix/bitmap.h>
#include <nuvix/spinlock.h>
#include <nuvix/vfs.h>
#include <nuvix/errno.h>
#include <nuvix/assert.h>

int fdtable_init(struct fdtable *fdt)
{
    struct file **files __free(kfree) = NULL;

    files = kzalloc(BITS_PER_LONG * sizeof(*files));
    if (!files)
        return -ENOMEM;

    spin_lock_init(&fdt->lock);
    bitmap_zero(&fdt->bitmap, BITS_PER_LONG);
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

int fd_alloc(struct fdtable *fdt)
{
    unsigned long flags;
    unsigned long fd;

    spin_lock_irqsave(&fdt->lock, flags);

    fd = find_first_zero_bit(&fdt->bitmap, BITS_PER_LONG);
    if (fd < BITS_PER_LONG)
        set_bit(fd, &fdt->bitmap);

    spin_unlock_irqrestore(&fdt->lock, flags);

    return fd == BITS_PER_LONG ? -EMFILE : (int)fd;
}

void fd_release(struct fdtable *fdt, unsigned long fd)
{
    unsigned long flags;

    spin_lock_irqsave(&fdt->lock, flags);
    Assert(fd < BITS_PER_LONG && test_bit(fd, &fdt->bitmap) && !fdt->files[fd],
           "fd release requires a reserved slot\n");
    clear_bit(fd, &fdt->bitmap);
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
        }
    }
    spin_unlock_irqrestore(&fdt->lock, flags);
    if (!file)
        return -EBADF;
    return fput(file);
}

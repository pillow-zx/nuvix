#ifndef _NUVIX_FDTABLE_H
#define _NUVIX_FDTABLE_H

#include <nuvix/assert.h>
#include <nuvix/spinlock.h>

struct file;

struct fdtable {
    spinlock_t lock;
    struct file **files;
    unsigned long bitmap;
};

#define stdin  0
#define stdout 1
#define stderr 2

int fdtable_init(struct fdtable *fdt);

void fdtable_destroy(struct fdtable *fdt);
void fdtable_clone(struct fdtable *dst, struct fdtable *src);

int fd_alloc(struct fdtable *fdt);

void fd_release(struct fdtable *fdt, unsigned long fd);

void fd_install(struct fdtable *fdt, unsigned long fd, struct file *file);

struct file *fd_open(struct fdtable *fdt, unsigned long fd);

int fd_close(struct fdtable *fdt, unsigned long fd);

#endif /* _NUVIX_FDTABLE_H */

#ifndef _NUVIX_FDTABLE_H
#define _NUVIX_FDTABLE_H

#include <nuvix/assert.h>
#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>

struct file;

struct fdtable {
    refcount_t refs;
    spinlock_t lock;
    struct file **files;
    unsigned long bitmap;
    unsigned long cloexec;
};

#define stdin  0
#define stdout 1
#define stderr 2

struct fdtable *fdtable_alloc(void);
struct fdtable *fdtable_clone(struct fdtable *src);

void fdtable_get(struct fdtable *fdt);
void fdtable_put(struct fdtable *fdt);

DECLARE_FREE(fdtable, struct fdtable *, fdtable_put(_T));

void fdtable_cloexec(struct fdtable *fdt);

int fd_alloc(struct fdtable *fdt);

int fd_alloc_from(struct fdtable *fdt, unsigned long min);

void fd_release(struct fdtable *fdt, unsigned long fd);

void fd_install(struct fdtable *fdt, unsigned long fd, struct file *file,
                bool cloexec);

struct file *fd_open(struct fdtable *fdt, unsigned long fd);

int fd_close(struct fdtable *fdt, unsigned long fd);

#define FD_CLOEXEC 1

void fd_set_cloexec(struct fdtable *fdt, unsigned long fd, bool enable);

bool fd_test_cloexec(struct fdtable *fdt, unsigned long fd);

#endif /* _NUVIX_FDTABLE_H */

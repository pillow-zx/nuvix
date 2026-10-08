#ifndef _NUVIX_FDTABLE_H
#define _NUVIX_FDTABLE_H

#include <nuvix/assert.h>
#include <nuvix/spinlock.h>
#include <nuvix/vector.h>

struct file;

struct fdtable {
    spinlock_t lock;
    /* Indexed by fd; NULL slots have no installed file. Protected by lock. */
    vector(struct file *) files;
};

/* Exclusive initialization; reserves empty slots for stdin/stdout/stderr. */
static inline int fdtable_init(struct fdtable *table)
{
    spin_lock_init(&table->lock);
    vector_init(table->files);
    return vector_resize(table->files, 3);
}

/* Exclusive teardown after all file references have been detached. */
static inline void fdtable_destroy(struct fdtable *table)
{
    for (size_t fd = 0; fd < vector_size(table->files); ++fd)
        Assert(!vector_at(table->files, fd),
               "fdtable destruction requires detached files\n");
    vector_destroy(table->files);
}

#endif /* _NUVIX_FDTABLE_H */

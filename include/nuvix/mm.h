#ifndef _NUVIX_MM_H
#define _NUVIX_MM_H

#include <nuvix/ctypes.h>
#include <nuvix/refcount.h>
#include <nuvix/cleanup.h>
#include <nuvix/mutex.h>
#include <nuvix/spinlock.h>
#include <nuvix/rbtree.h>

struct mm_struct {
    refcount_t refcount;

    /* Protects VMA layout and ordinary fault submission. */
    struct mutex mmap_lock;
    struct rb_root vma_tree;

    /* Protects this mm's software page-table access and detachment. */
    spinlock_t pgtable_lock;
    /* Physical root address; high-half entries share permanent kernel tables. */
    paddr_t pgd;
};

/*
 * After slub_init() and pgtable_use_buddy(). Returns an unpublished, empty mm
 * with an independent root and one caller reference, or NULL on allocation
 * failure. The mm owns the root's page-table ownership reference.
 */
struct mm_struct *mm_alloc(void);
/* Requires a held reference or equivalent lifetime protection. */
void mm_get(struct mm_struct *mm);
/*
 * Final put requires an empty VMA tree and user half, no task/CPU/software
 * users, and completed TLB synchronization. Call outside short locks;
 * CPU root users must retain an mm reference until switching away.
 */
void mm_put(struct mm_struct *mm);

DECLARE_FREE(mm_put, struct mm_struct *, if (_T) mm_put(_T));

#endif /* _NUVIX_MM_H */

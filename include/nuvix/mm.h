#ifndef _NUVIX_MM_H
#define _NUVIX_MM_H

#include <nuvix/ctypes.h>
#include <nuvix/refcount.h>
#include <nuvix/cleanup.h>
#include <nuvix/mutex.h>
#include <nuvix/spinlock.h>
#include <nuvix/rbtree.h>
#include <nuvix/bitops.h>

#define VM_READ       BIT(0)
#define VM_WRITE      BIT(1)
#define VM_EXEC       BIT(2)

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

struct vm_struct {
    /* Borrowed while linked; NULL while caller-owned. */
    struct mm_struct *vm_mm;
    /* [vm_start, vm_end); page-aligned while linked. */
    vaddr_t vm_start;
    vaddr_t vm_end;
    unsigned long vm_flags;
    struct rb_node vm_node;
    struct file *vm_file;
    uint64_t vm_offset;
    /* [vm_fstart, vm_fend); file-backed virtual range. */
    vaddr_t vm_fstart;
    vaddr_t vm_fend;
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
 * Final put frees owned VMAs, user mappings and the root. Requires no
 * task/CPU/software users and completed TLB synchronization. Call outside
 * short locks. CPU root users must retain an mm reference until switching away.
 */
void mm_put(struct mm_struct *mm);

DECLARE_FREE(mm_put, struct mm_struct *, if (_T) mm_put(_T));

struct vm_struct *vma_alloc(void);
/* Requires a detached, caller-owned VMA. */
void vma_free(struct vm_struct *vma);

DECLARE_FREE(vma, struct vm_struct *, if (_T) vma_free(_T));

/*
 * Tree operations require mmap_lock, except during exclusive final mm
 * destruction. Returned VMAs are borrowed and cannot be retained across unlock.
 * Linked bounds and tree membership must be changed through remove/insert.
 * These operations change metadata only, not PTEs or TLBs.
 */
/* Returns only the VMA containing addr, or NULL for a hole. */
struct vm_struct *vma_find(struct mm_struct *mm, vaddr_t addr);
/* First VMA intersecting [start, end), or NULL; requires start < end. */
struct vm_struct *vma_find_intersection(struct mm_struct *mm, vaddr_t start,
                                        vaddr_t end);
/*
 * Inserts a detached VMA. Success transfers ownership to mm;
 * -EINVAL for invalid bounds/flags and -EEXIST for overlap leave it caller-owned.
 */
int vma_insert(struct mm_struct *mm, struct vm_struct *vma);
/* Detaches a linked VMA and transfers ownership back to the caller. */
void vma_remove(struct vm_struct *vma);

struct vm_struct *map_range(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                            unsigned long vm_flags);
void unmap_range(struct mm_struct *mm, vaddr_t start, vaddr_t end);

int handle_page_fault(struct mm_struct *mm, vaddr_t addr, unsigned long access);

#endif /* _NUVIX_MM_H */

#ifndef _NUVIX_MM_H
#define _NUVIX_MM_H

#include <nuvix/ctypes.h>
#include <nuvix/refcount.h>
#include <nuvix/cleanup.h>
#include <nuvix/mutex.h>
#include <nuvix/spinlock.h>
#include <nuvix/rbtree.h>
#include <nuvix/bitops.h>

#define VM_READ          BIT(0)
#define VM_WRITE         BIT(1)
#define VM_EXEC          BIT(2)

#define PROT_NONE        0x0
#define PROT_READ        0x1
#define PROT_WRITE       0x2
#define PROT_EXEC        0x4

#define MAP_PRIVATE      0x02
#define MAP_FIXED        0x10
#define MAP_ANONYMOUS    0x20

#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4

struct mm_struct {
    refcount_t urefs;
    /* One shared address-space reference, plus CPU root references. */
    refcount_t refs;

    /* Protects VMA layout and ordinary fault submission. */
    struct mutex mmap_lock;
    struct rb_root vma_tree;
    vaddr_t start_brk;
    vaddr_t brk;

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
 * After slub_init(). Returns an unpublished, empty mm
 * with an independent root and one caller user reference, or NULL on allocation
 * failure. The users collectively hold one mm lifetime reference.
 */
struct mm_struct *mm_alloc(void);

int mm_clone(struct mm_struct *dst, struct mm_struct *src);

void switch_mm(struct mm_struct *mm);

void mm_get(struct mm_struct *mm);
/*
 * Sleepable context. The final user put releases owned VMAs and their file
 * references, then drops the users' collective mm lifetime reference.
 */
void mm_put(struct mm_struct *mm);

DECLARE_FREE(mm_put, struct mm_struct *, if (_T) mm_put(_T));

/* CPU root references keep the mm, user mappings and root alive independently. */
void mm_grab(struct mm_struct *mm);
/* Drop a lifetime reference; CPU holders first switch away and flush the TLB. */
void mm_drop(struct mm_struct *mm);

DECLARE_FREE(mm_drop, struct mm_struct *, if (_T) mm_drop(_T));

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
int unmap_range(struct mm_struct *mm, vaddr_t start, vaddr_t end);
int move_range(struct mm_struct *mm, vaddr_t start, vaddr_t end, vaddr_t nstart,
               vaddr_t nend);
int protect_range(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                  unsigned long vm_flags);

int handle_page_fault(struct mm_struct *mm, vaddr_t addr, unsigned long access);

#endif /* _NUVIX_MM_H */

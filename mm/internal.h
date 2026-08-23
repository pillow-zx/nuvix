#ifndef _NUVIX_MM_INTERNAL_H
#define _NUVIX_MM_INTERNAL_H

#include <nuvix/mm.h>
#include <nuvix/bitops.h>
#include <nuvix/cleanup.h>
#include <nuvix/refcount.h>
#include <nuvix/mutex.h>
#include <nuvix/tools.h>
#include <nuvix/types.h>
#include <uapi/mman.h>

#include <nuvix/page.h>
#include <nuvix/pgtable.h>

struct file;

#define VM_READ	 BIT_U32(0)
#define VM_WRITE BIT_U32(1)
#define VM_EXEC	 BIT_U32(2)

#define VMA_CODE  BIT_U32(0)
#define VMA_HEAP  BIT_U32(1)
#define VMA_STACK BIT_U32(2)
#define VMA_MMAP  BIT_U32(3)

#define NR_VMA 16

#define USER_FAULT_READ	 0
#define USER_FAULT_WRITE 1
#define USER_FAULT_EXEC	 2

struct vm_area_struct {
	uintptr_t vm_start;
	uintptr_t vm_end;
	uint32_t vm_flags;
	uint32_t vm_type;
	struct file *vm_file;
	uint64_t vm_offset;
	bool vm_shared;
	bool used;
};

struct mm_struct {
	refcount_t refcount;
	mutex_t mmap_lock;
	pte_t *pgd;
	uintptr_t brk;
	uintptr_t code_start;
	uintptr_t code_end;
	uint32_t membarrier_registrations;
	struct vm_area_struct vma[NR_VMA];
};

struct mm_mapping_release {
	paddr_t pa;
	bool dirty;
};

struct mm_teardown {
	struct mm_mapping_release *release;
	size_t nr_release;
	size_t release_capacity;
};

static_assert(NR_VMA > 0, "NR_VMA must stay positive");

__always_inline __must_check __const
static inline uintptr_t mm_page_align_up(uintptr_t addr)
{
	return ALIGN_UP(addr, PAGE_SIZE);
}

__always_inline __must_check __pure __nonnull(1)
static inline uint64_t vma_offset_at(const struct vm_area_struct *vma,
				const uintptr_t va)
{
	return vma->vm_offset + (va - vma->vm_start);
}

__always_inline __must_check __pure __nonnull(1)
static inline uint64_t vma_page_index(const struct vm_area_struct *vma,
		const uintptr_t page_addr)
{
	uintptr_t base = vma->vm_start & PAGE_MASK;
	uint64_t file_base = vma->vm_offset & PAGE_MASK;

	return (file_base + (page_addr - base)) / PAGE_SIZE;
}

__always_inline __nonnull(1)
static inline void mm_lock(struct mm_struct *mm)
{
	mutex_lock(&mm->mmap_lock);
}

__always_inline __nonnull(1)
static inline void mm_unlock(struct mm_struct *mm)
{
	mutex_unlock(&mm->mmap_lock);
}

SCOPE_GUARD_DEFINE(mm_guard, struct mm_struct *, mm_lock(_T), mm_unlock(_T))

__always_inline __must_check __const
static inline int vma_capacity(void)
{
	return NR_VMA;
}

__always_inline __must_check __const
static inline bool mm_prot_is_valid(int prot)
{
	return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

__always_inline __must_check __const
static inline uint32_t mm_prot_to_vm_flags(int prot)
{
	uint32_t flags = 0;

	if (prot & PROT_READ)
		flags |= VM_READ;
	if (prot & PROT_WRITE)
		flags |= VM_READ | VM_WRITE;
	if (prot & PROT_EXEC)
		flags |= VM_EXEC;

	return flags;
}

__always_inline __must_check __const
static inline pgprot_t mm_prot_to_pte_flags(int prot)
{
	return pgprot_user((prot & PROT_READ) != 0, (prot & PROT_WRITE) != 0,
			   (prot & PROT_EXEC) != 0);
}

__always_inline __must_check __const
static inline pgprot_t vma_flags_to_pte(uint32_t vm_flags)
{
	return pgprot_user((vm_flags & VM_READ) != 0,
			   (vm_flags & VM_WRITE) != 0,
			   (vm_flags & VM_EXEC) != 0);
}

/* COW view of a leaf protection: readable, never writable. */
__always_inline __must_check __const
static inline pgprot_t pgprot_make_readonly(pgprot_t prot)
{
	return prot & ~PTE_W;
}

void mm_pte_mapping_get(paddr_t pa);
void mm_pte_mapping_put(const struct vm_area_struct *vma, paddr_t pa);
void mm_pte_mapping_put_dirty(paddr_t pa, bool dirty);

__always_inline __must_check __pure __nonnull(1)
static inline bool vma_overlaps(const struct vm_area_struct *vma,
		const uintptr_t start, const uintptr_t end)
{
	return vma->used && start < vma->vm_end && end > vma->vm_start;
}

__always_inline __must_check __pure __nonnull(1)
static inline bool vma_contains_split_addr(const struct vm_area_struct *vma,
		const uintptr_t addr)
{
	return vma->used && addr > vma->vm_start && addr < vma->vm_end;
}

__always_inline __must_check __pure __nonnull(1)
static inline bool vma_is_anonymous(const struct vm_area_struct *vma)
{
	return !vma->vm_file &&
	       (vma->vm_type == VMA_HEAP || vma->vm_type == VMA_STACK ||
		vma->vm_type == VMA_MMAP);
}

__always_inline __must_check __pure
static inline bool vma_covers_range(const struct vm_area_struct *vma,
		const uintptr_t start, const uintptr_t end)
{
	return vma && vma->used && start >= vma->vm_start && end <= vma->vm_end;
}

__must_check
struct mm_struct *mm_alloc(void);

__cold
void mm_destroy(struct mm_struct *mm);

__must_check
pte_t *mm_create_user_pgd(void);

__must_check
struct vm_area_struct *find_vma(struct mm_struct *mm, uintptr_t addr);

__must_check
int mm_range_end_page_aligned(uintptr_t start, size_t length, uintptr_t *end);

__must_check __nonnull(1)
int fault_in_user_range(struct mm_struct *mm, uintptr_t addr, size_t size, int access);

/* Caller holds mm->mmap_lock across the call and must run
 * mm_teardown_release() after unlocking. */
__must_check __nonnull(1, 5)
int fault_in_user_range_locked(struct mm_struct *mm, uintptr_t addr, size_t size,
			       int access, struct mm_teardown *teardown);

/* One uaccess lifetime transaction: pins the MM with its own reference,
 * holds mmap_lock across fault-in and the actual user accesses, and
 * defers mapping-reference release to transaction end.  The exception
 * table only turns a faulting access into an error; mapping lifetime
 * comes exclusively from the lock held by this transaction. */
struct uaccess_txn {
	struct mm_struct *mm;
	struct mm_teardown teardown;
};

/* Take the current Task's Proc MM reference and acquire mmap_lock.
 * Returns 0 with the lock held, or a negative errno with the
 * transaction left inert. */
__must_check __nonnull(1)
int uaccess_txn_begin(struct uaccess_txn *txn);

/* Release mmap_lock, drop deferred mapping references outside the lock,
 * then drop the MM reference.  Safe on an inert transaction. */
__nonnull(1)
void uaccess_txn_end(struct uaccess_txn *txn);

__must_check __nonnull(1)
struct vm_area_struct *vma_alloc_slot(struct mm_struct *mm);

void vma_free_slot(struct vm_area_struct *vma);

__must_check __pure __nonnull(1)
int vma_free_slot_count(struct mm_struct *mm);

__must_check __pure __nonnull(1)
bool vma_range_overlaps(struct mm_struct *mm, uintptr_t start, uintptr_t end);

__must_check __pure __nonnull(1, 2)
bool vma_range_overlaps_other(struct mm_struct *mm, const struct vm_area_struct *skip,
		uintptr_t start, uintptr_t end);

__must_check __nonnull(1, 2)
int vma_split_at(struct mm_struct *mm, struct vm_area_struct *vma, uintptr_t addr);

__nonnull(1)
void vma_merge_all(struct mm_struct *mm);

__must_check __pure __nonnull(1)
int vma_munmap_slots_needed(struct mm_struct *mm, uintptr_t start, uintptr_t end);

__must_check __pure __nonnull(1)
int vma_mprotect_slots_needed(struct mm_struct *mm, uintptr_t start, uintptr_t end);

__must_check __pure __nonnull(1)
bool vma_range_is_mapped(struct mm_struct *mm, uintptr_t start, uintptr_t end);

__must_check __nonnull(1)
int vma_split_range(struct mm_struct *mm, uintptr_t start, uintptr_t end);

__nonnull(1)
void vma_update_flags_range(struct mm_struct *mm, uintptr_t start, uintptr_t end, uint32_t vm_flags);

__must_check __nonnull(1, 2)
int vma_unmap_range(struct mm_struct *mm, struct vm_area_struct *vma, uintptr_t start,
		uintptr_t end, uintptr_t *unmap_start, uintptr_t *unmap_end);

__nonnull(1, 2, 5)
void mm_unmap_user_pages_locked(struct mm_struct *mm, const struct vm_area_struct *vma,
		uintptr_t start, uintptr_t end, struct mm_teardown *teardown);

__must_check
int mm_teardown_reserve_range(struct mm_struct *mm, uintptr_t start, uintptr_t end, struct mm_teardown *teardown);

__nonnull(1)
void mm_teardown_sync(struct mm_struct *mm, struct mm_teardown *teardown, bool flush_icache);

void mm_teardown_release(struct mm_teardown *teardown);

__nonnull(1, 4)
void mm_replace_user_pte_locked(struct mm_struct *mm, const struct vm_area_struct *vma,
		uintptr_t va, pte_t *pte, pte_t new_entry, paddr_t old_pa, struct mm_teardown *teardown);

__must_check
int mm_move_user_pages_locked(struct mm_struct *mm, uintptr_t old_start, uintptr_t new_start, size_t len);

#endif

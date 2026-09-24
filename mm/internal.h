#ifndef _NUVIX_MM_INTERNAL_H
#define _NUVIX_MM_INTERNAL_H

#include <nuvix/mm.h>
#include <nuvix/atomic.h>
#include <nuvix/bitops.h>
#include <nuvix/cleanup.h>
#include <nuvix/refcount.h>
#include <nuvix/mutex.h>
#include <nuvix/tools.h>
#include <nuvix/types.h>
#include <nuvix/rbtree.h>
#include <uapi/mman.h>

#include <nuvix/page.h>
#include <nuvix/pgtable.h>

struct file;

struct mm_page_slot {
	struct rb_node node;
	uintptr_t index;
	struct page *page;
	bool cow;
};

struct anon_shared {
	refcount_t refs;
	spinlock_t lock;
	struct rb_root pages;
	struct list_head mappings;
	/* A kernel-owned single page stays resident without VMA coverage. */
	bool kernel_page;
};

#define VM_READ	  BIT_U32(0)
#define VM_WRITE  BIT_U32(1)
#define VM_EXEC	  BIT_U32(2)
#define VMA_CODE  BIT_U32(0)
#define VMA_HEAP  BIT_U32(1)
#define VMA_STACK BIT_U32(2)
#define VMA_MMAP  BIT_U32(3)

#define NR_MM_REGIONS 4

#define USER_FAULT_READ	 0
#define USER_FAULT_WRITE 1
#define USER_FAULT_EXEC	 2

enum mm_region_kind {
	MM_REGION_RESERVED,
	MM_REGION_FIXED,
};

struct mm_region {
	vaddr_t start;
	vaddr_t end;
	enum mm_region_kind kind;
	bool used;
};

struct mm_layout {
	struct mm_region regions[NR_MM_REGIONS];
};

enum mm_lifecycle {
	MM_LIFECYCLE_BUILDING,
	MM_LIFECYCLE_ACTIVE,
	MM_LIFECYCLE_RETIRING,
};

struct vm_area_struct {
	struct rb_node node;
	struct list_head spare;
	/* Shared-object coverage, protected by anon_shared.lock. */
	struct list_head anon_link;
	uint64_t anon_first;
	uint64_t anon_end;
	bool anon_registered;
	struct mm_struct *owner;
	uintptr_t vm_start;
	uintptr_t vm_end;
	uint32_t vm_flags;
	uint32_t vm_type;
	struct file *vm_file;
	struct anon_shared *vm_anon;
	uint64_t vm_offset;
	bool vm_shared;
	bool used;
};

struct mm_struct {
	refcount_t refcount;
	struct list_head retirement;
	/*
	 * The high 32 bits encode enum mm_lifecycle and the low 32 bits count
	 * Proc publications.  Keeping both in one atomic word makes publishing
	 * and withdrawing the last Proc owner one indivisible state transition.
	 */
	atomic64_t lifecycle;
	mutex_t mmap_lock;
	pte_t *pgroot;
	struct rb_root private;
	struct mm_layout layout;
	uintptr_t brk;
	uintptr_t code_start;
	uintptr_t code_end;
	uint64_t map_sequence;
	struct rb_root vmas;
	struct list_head vma_spares;
};

static_assert(NR_MM_REGIONS > 0, "NR_MM_REGIONS must stay positive");

__must_check __pure __nonnull(1)
static inline uint64_t vma_offset_at(const struct vm_area_struct *vma, const uintptr_t va)
{
	return vma->vm_offset + (va - vma->vm_start);
}

__must_check __pure __nonnull(1)
static inline uint64_t vma_page_index(const struct vm_area_struct *vma, const uintptr_t page_addr)
{
	uintptr_t base = vma->vm_start & PAGE_MASK;
	uint64_t file_base = vma->vm_offset & PAGE_MASK;

	return (file_base + (page_addr - base)) / PAGE_SIZE;
}

__nonnull(1) __nonnull(1)
static inline void mm_lock(struct mm_struct *mm)
{
	mutex_lock(&mm->mmap_lock);
}

__nonnull(1) __nonnull(1)
static inline void mm_unlock(struct mm_struct *mm)
{
	mutex_unlock(&mm->mmap_lock);
}

SCOPE_GUARD_DEFINE(mm_guard, struct mm_struct *, mm_lock(_T), mm_unlock(_T))

/*
 * Layout reservations are constructed while an address space is unpublished.
 * Published-address-space range queries require mmap_lock when concurrent
 * layout mutation is possible.
 */
__must_check __nonnull(1)
int mm_layout_reserve(struct mm_struct *mm, vaddr_t start, vaddr_t end, enum mm_region_kind kind);

__nonnull(1)
void mm_layout_release(struct mm_struct *mm, vaddr_t start, vaddr_t end, enum mm_region_kind kind);

__must_check __nonnull(1)
int mm_layout_init(struct mm_struct *mm);

__must_check __pure __nonnull(1)
bool mm_layout_contains(const struct mm_struct *mm, vaddr_t addr);

__must_check __pure __nonnull(1)
bool mm_layout_overlaps(const struct mm_struct *mm, vaddr_t start, vaddr_t end);

struct vm_area_struct *vma_first(struct mm_struct *mm);

struct vm_area_struct *vma_next(struct vm_area_struct *vma);

#define for_each_vma(vma, mm)                                                  \
	for (struct vm_area_struct *vma = vma_first(mm),                       \
				   *_vma##_next = vma_next(vma);               \
	     vma; vma = _vma##_next, _vma##_next = vma_next(vma))

int vma_reserve(struct mm_struct *mm, int count);

void vma_publish(struct vm_area_struct *vma);

void vma_discard_spares(struct mm_struct *mm);

__must_check __const
static inline bool mm_root_is_valid(int proot)
{
	return (proot & ~(PROOT_READ | PROOT_WRITE | PROOT_EXEC)) == 0;
}

__must_check __const
static inline uint32_t mm_root_to_vm_flags(int proot)
{
	uint32_t flags = 0;

	if (proot & PROOT_READ)
		flags |= VM_READ;
	if (proot & PROOT_WRITE)
		flags |= VM_READ | VM_WRITE;
	if (proot & PROOT_EXEC)
		flags |= VM_EXEC;

	return flags;
}

__must_check __const
static inline pgprot_t mm_root_to_pte_flags(int proot)
{
	return pgprot_user((proot & PROOT_READ) != 0, (proot & PROOT_WRITE) != 0,
		       (proot & PROOT_EXEC) != 0);
}

__must_check __const
static inline pgprot_t vma_flags_to_pte(uint32_t vm_flags)
{
	return pgprot_user((vm_flags & VM_READ) != 0, (vm_flags & VM_WRITE) != 0, (vm_flags & VM_EXEC) != 0);
}

void pte_mapping_get(paddr_t pa);

void pte_mapping_put(paddr_t pa);

__must_check __pure __nonnull(1)
static inline bool vma_overlaps(const struct vm_area_struct *vma, const uintptr_t start, const uintptr_t end)
{
	return vma->used && start < vma->vm_end && end > vma->vm_start;
}

__must_check __pure __nonnull(1)
static inline bool vma_contains_split_addr(const struct vm_area_struct *vma, const uintptr_t addr)
{
	return vma->used && addr > vma->vm_start && addr < vma->vm_end;
}

__must_check __pure __nonnull(1)
static inline bool vma_is_anonymous(const struct vm_area_struct *vma)
{
	return !vma->vm_file &&
	       (vma->vm_type == VMA_HEAP || vma->vm_type == VMA_STACK ||
		vma->vm_type == VMA_MMAP);
}

__must_check __pure
static inline bool vma_covers_range(const struct vm_area_struct *vma, const uintptr_t start,
                const uintptr_t end)
{
	return vma && vma->used && start >= vma->vm_start && end <= vma->vm_end;
}

__must_check __malloc
struct mm_struct *mm_alloc(void);

__cold
void mm_destroy(struct mm_struct *mm);

__cold __nonnull(1)
void destroy_mappings(struct mm_struct *mm);

__must_check __malloc
pte_t *create_pgroot(struct mm_struct *mm);

__must_check
struct vm_area_struct *find_vma(struct mm_struct *mm, uintptr_t addr);

__must_check
int mm_range_end_page_aligned(uintptr_t start, size_t length, uintptr_t *end);

__must_check __nonnull(1)
int fault_in_user_range(struct mm_struct *mm, uintptr_t addr, size_t size, int access);

/* Caller holds mm->mmap_lock across the call and must run
 * mm_teardown_release() after unlocking. */
__must_check __nonnull(1, 5)
int fault_in_urange_locked(struct mm_struct *mm, uintptr_t addr, size_t size, int access, struct mm_teardown *teardown);

__must_check __nonnull(1)
struct vm_area_struct *vma_alloc_slot(struct mm_struct *mm);

void vma_free_slot(struct vm_area_struct *vma);

__must_check __pure __nonnull(1)
int vma_free_slot_count(struct mm_struct *mm);

__must_check __pure __nonnull(1)
bool vma_range_overlaps(struct mm_struct *mm, uintptr_t start, uintptr_t end);

__must_check __pure __nonnull(1, 2)
bool vma_range_overlaps_other(struct mm_struct *mm, const struct vm_area_struct *skip, uintptr_t start, uintptr_t end);

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
int vma_unmap_range(struct mm_struct *mm, struct vm_area_struct *vma,
		uintptr_t start, uintptr_t end, uintptr_t *unmap_start, uintptr_t *unmap_end);

__nonnull(1, 2, 5)
void unmap_pages_locked(struct mm_struct *mm, const struct vm_area_struct *vma,
		uintptr_t start, uintptr_t end, struct mm_teardown *teardown);

__nonnull(1)
void mm_teardown_sync(struct mm_struct *mm, struct mm_teardown *teardown, bool flush_icache);

void mm_teardown_release(struct mm_teardown *teardown);

__nonnull(1, 4)
void replace_pte_locked(struct mm_struct *mm, const struct vm_area_struct *vma,
                uintptr_t va, pte_t *pte, pte_t new_entry, paddr_t old_pa, struct mm_teardown *teardown);

struct mm_page_slot *mm_private_find(struct mm_struct *mm, uintptr_t va);
int mm_private_set(struct mm_struct *mm, uintptr_t va, struct page *page,
		   bool cow);
void mm_private_remove(struct mm_struct *mm, uintptr_t start, uintptr_t end);
int mm_private_clone(struct mm_struct *child, struct mm_struct *parent);
__must_check __malloc
struct anon_shared *anon_shared_create(void);
/* Caller holds the owning mm lock; unregister follows PTE retirement. */
void anon_shared_register(struct vm_area_struct *vma);
void anon_shared_update(struct vm_area_struct *vma);
void anon_shared_unregister(struct vm_area_struct *vma);
struct page *mm_zero_page(void);
struct page *anon_shared_page(struct anon_shared *anon, uintptr_t index);

__must_check __nonnull(1)
int map_pte_like(pte_t *root, uintptr_t va, paddr_t pa, pte_t old_entry);

#endif

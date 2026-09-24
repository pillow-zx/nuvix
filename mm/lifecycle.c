/*
 * mm/address_space.c - AddressSpace construction, lifecycle, and layout
 */

#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/wait.h>
#include <nuvix/sched.h>

#include "internal.h"

#define MM_LIFECYCLE_STATE_SHIFT 32

/* Last-reference ownership is transferred through this embedded queue.
 * The existing process reaper drains it without holding scheduler locks. */
static LIST_HEAD(mm_retired);
static DEFINE_SPINLOCK(mm_retired_lock);

static int64_t mm_lifecycle_pack(enum mm_lifecycle state, uint32_t publishers)
{
	return ((int64_t)(uint32_t)state << MM_LIFECYCLE_STATE_SHIFT) |
	       (uint32_t)publishers;
}

static enum mm_lifecycle mm_lifecycle_state_value(int64_t value)
{
	return (enum mm_lifecycle)((uint64_t)value >> MM_LIFECYCLE_STATE_SHIFT);
}

static enum mm_lifecycle mm_lifecycle_state(const struct mm_struct *mm)
{
	return (enum mm_lifecycle)((uint64_t)(atomic64_read_acquire(&mm->lifecycle) >> MM_LIFECYCLE_STATE_SHIFT));
}

static uint32_t mm_lifecycle_publish(int64_t value)
{
	return (uint32_t)(uint64_t)value;
}

static bool mm_region_overlaps(const struct mm_region *region, vaddr_t start,
			       vaddr_t end)
{
	return region->used && start < region->end && end > region->start;
}

int mm_layout_init(struct mm_struct *mm)
{
	return mm_layout_reserve(mm, USER_STACK_GUARD_BASE, USER_STACK_BASE,
				 MM_REGION_RESERVED);
}

int mm_layout_reserve(struct mm_struct *mm, vaddr_t start, vaddr_t end,
		      enum mm_region_kind kind)
{
	if (mm_lifecycle_state(mm) != MM_LIFECYCLE_BUILDING)
		return -EINVAL;
	if (start == 0 || start >= end || end > TASK_SIZE)
		return -EINVAL;
	if (kind != MM_REGION_RESERVED && kind != MM_REGION_FIXED)
		return -EINVAL;

	for (int i = 0; i < NR_MM_REGIONS; i++) {
		struct mm_region *region = &mm->layout.regions[i];

		if (!region->used)
			continue;
		if (region->start == start && region->end == end &&
		    region->kind == kind)
			return 0;
		if (mm_region_overlaps(region, start, end))
			return -EINVAL;
	}

	for (int i = 0; i < NR_MM_REGIONS; i++) {
		struct mm_region *region = &mm->layout.regions[i];

		if (region->used)
			continue;
		region->start = start;
		region->end = end;
		region->kind = kind;
		region->used = true;
		return 0;
	}

	return -ENOMEM;
}

void mm_layout_release(struct mm_struct *mm, vaddr_t start, vaddr_t end,
		       enum mm_region_kind kind)
{
	for (int i = 0; i < NR_MM_REGIONS; i++) {
		struct mm_region *region = &mm->layout.regions[i];

		if (!region->used || region->start != start ||
		    region->end != end || region->kind != kind)
			continue;
		memset(region, 0, sizeof(*region));
		return;
	}
}

bool mm_layout_contains(const struct mm_struct *mm, vaddr_t addr)
{
	for (int i = 0; i < NR_MM_REGIONS; i++) {
		const struct mm_region *region = &mm->layout.regions[i];

		if (region->used && addr >= region->start && addr < region->end)
			return true;
	}

	return false;
}

bool mm_layout_overlaps(const struct mm_struct *mm, vaddr_t start, vaddr_t end)
{
	if (start >= end)
		return false;

	for (int i = 0; i < NR_MM_REGIONS; i++) {
		if (mm_region_overlaps(&mm->layout.regions[i], start, end))
			return true;
	}

	return false;
}

int mm_install_fixed_page(struct mm_struct *mm, uintptr_t va, void *page,
			  int prot)
{
	uintptr_t end;
	pte_t *pte;
	int ret;

	if (!mm || mm_lifecycle_state(mm) != MM_LIFECYCLE_BUILDING ||
	    !mm->pgroot || !page || (va & (PAGE_SIZE - 1)) || va >= TASK_SIZE ||
	    PAGE_SIZE > TASK_SIZE - va || !mm_root_is_valid(prot))
		return -EINVAL;
	end = va + PAGE_SIZE;

	mm_lock(mm);
	if (mm_lifecycle_state(mm) != MM_LIFECYCLE_BUILDING ||
	    mm_layout_overlaps(mm, va, end) ||
	    vma_range_overlaps(mm, va, end)) {
		ret = -EINVAL;
		goto out;
	}

	pte = pt_lookup(mm->pgroot, va);
	if (pte && pte_present(*pte)) {
		ret = -EEXIST;
		goto out;
	}

	ret = mm_layout_reserve(mm, va, end, MM_REGION_FIXED);
	if (ret < 0)
		goto out;

	ret = map_page(mm->pgroot, va, __pa((uintptr_t)page),
		       mm_root_to_pte_flags(prot));
	if (ret < 0)
		mm_layout_release(mm, va, end, MM_REGION_FIXED);
	else
		page_get(virt_to_page(page));
out:
	mm_unlock(mm);
	return ret;
}

struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm = kmalloc(sizeof(struct mm_struct), ALLOC_NOWAIT);

	if (!mm)
		return NULL;

	memset(mm, 0, sizeof(struct mm_struct));
	refcount_set(&mm->refcount, 1);
	atomic64_set_relaxed(&mm->lifecycle,
			     mm_lifecycle_pack(MM_LIFECYCLE_BUILDING, 0));
	INIT_LIST_HEAD(&mm->vma_spares);
	mutex_init(&mm->mmap_lock);
	if (mm_layout_init(mm) < 0) {
		mm_put(mm);
		return NULL;
	}
	return mm;
}

struct mm_struct *mm_create(void)
{
	struct mm_struct *mm = mm_alloc();

	if (!mm)
		return NULL;

	mm->pgroot = create_pgroot(mm);
	if (!mm->pgroot) {
		mm_put(mm);
		return NULL;
	}

	return mm;
}

void mm_publish(struct mm_struct *mm)
{
	for (;;) {
		int64_t old = atomic64_read_acquire(&mm->lifecycle);
		enum mm_lifecycle state = mm_lifecycle_state_value(old);
		uint32_t publishers = mm_lifecycle_publish(old);
		int64_t desired;

		BUG_ON(state != MM_LIFECYCLE_BUILDING &&
		       state != MM_LIFECYCLE_ACTIVE);
		BUG_ON(publishers == REFCOUNT_MAX);
		desired =
			mm_lifecycle_pack(MM_LIFECYCLE_ACTIVE, publishers + 1);
		if (atomic64_cmpxchg_acq_rel(&mm->lifecycle, old, desired) ==
		    old)
			return;
	}
}

void mm_unpublish(struct mm_struct *mm)
{
	for (;;) {
		int64_t old = atomic64_read_acquire(&mm->lifecycle);
		enum mm_lifecycle state = mm_lifecycle_state_value(old);
		uint32_t publishers = mm_lifecycle_publish(old);
		enum mm_lifecycle next_state;
		int64_t desired;

		BUG_ON(state != MM_LIFECYCLE_ACTIVE);
		BUG_ON(publishers == 0);
		next_state = publishers == 1 ? MM_LIFECYCLE_RETIRING
					     : MM_LIFECYCLE_ACTIVE;
		desired = mm_lifecycle_pack(next_state, publishers - 1);
		if (atomic64_cmpxchg_acq_rel(&mm->lifecycle, old, desired) ==
		    old)
			return;
	}
}

static void mm_begin_retirement(struct mm_struct *mm)
{
	for (;;) {
		int64_t old = atomic64_read_acquire(&mm->lifecycle);
		enum mm_lifecycle state = mm_lifecycle_state_value(old);
		uint32_t publishers = mm_lifecycle_publish(old);
		int64_t desired;

		BUG_ON(publishers != 0);
		BUG_ON(state != MM_LIFECYCLE_BUILDING &&
		       state != MM_LIFECYCLE_RETIRING);
		if (state == MM_LIFECYCLE_RETIRING)
			return;
		desired = mm_lifecycle_pack(MM_LIFECYCLE_RETIRING, 0);
		if (atomic64_cmpxchg_acq_rel(&mm->lifecycle, old, desired) ==
		    old)
			return;
	}
}

void mm_get(struct mm_struct *mm)
{
	if (mm)
		refcount_inc(&mm->refcount);
}

void mm_put(struct mm_struct *mm)
{
	if (!mm)
		return;

	if (!refcount_dec_and_test(&mm->refcount))
		return;
	if (wait_may_block()) {
		mm_destroy(mm);
	} else {
		irq_flags_t flags;

		spin_lock_irqsave(&mm_retired_lock, flags);
		list_add_tail(&mm->retirement, &mm_retired);
		spin_unlock_irqrestore(&mm_retired_lock, flags);
		sched_notify_reaper();
	}
}

int mm_refcount_read(const struct mm_struct *mm)
{
	return refcount_read(&mm->refcount);
}

uintptr_t mm_pgtable_token(const struct mm_struct *mm)
{
	BUG_ON(!mm || !mm->pgroot);
	return pgtable_token(mm->pgroot);
}

__must_check __nonnull(1)
int map_pte_like(pte_t *root, uintptr_t va, paddr_t pa, pte_t old_entry)
{
	pgprot_t perm = pte_prot(old_entry);
	int ret;
	pte_t *pte;

	ret = map_page(root, va, pa,
		       pte_present(old_entry) ? perm
					      : pgprot_user(true, false, false));
	if (ret < 0)
		return ret;

	pte = pt_lookup(root, va);
	BUG_ON(!pte);
	*pte = pte_make(pa, perm);
	return 0;
}

static pte_t mm_private_child_pte(pte_t entry)
{
	if (!pte_present(entry))
		return entry;

	return pte_make(pte_phys(entry), pgprot_ro(pte_prot(entry)));
}

struct mm_struct *dup_mm(struct mm_struct *oldmm)
{
	struct mm_struct *newmm;
	bool parent_downgraded = false;

	if (!oldmm)
		return NULL;
	newmm = mm_create();
	if (!newmm)
		return NULL;

	mm_lock(oldmm);
	newmm->brk = oldmm->brk;
	newmm->code_start = oldmm->code_start;
	newmm->code_end = oldmm->code_end;
	for_each_vma (vma, oldmm) {
		struct vm_area_struct *copy = vma_alloc_slot(newmm);

		if (!copy)
			goto fail;
		*copy = *vma;
		copy->owner = newmm;
		copy->used = false;
		copy->anon_registered = false;
		file_get(copy->vm_file);
		anon_shared_get(copy->vm_anon);
		vma_publish(copy);
	}
	for (int i = 0; i < NR_MM_REGIONS; i++) {
		struct mm_region *region = &oldmm->layout.regions[i];

		if (!region->used || region->kind != MM_REGION_FIXED)
			continue;
		if (mm_layout_reserve(newmm, region->start, region->end,
				      MM_REGION_FIXED) < 0)
			goto fail;
		for (uintptr_t va = region->start; va < region->end;
		     va += PAGE_SIZE) {
			pte_t *pte = pt_lookup(oldmm->pgroot, va);

			if (!pte || !pte_upage(*pte))
				continue;
			if (map_pte_like(newmm->pgroot, va, pte_phys(*pte),
					  *pte) < 0)
				goto fail;
			pte_mapping_get(pte_phys(*pte));
		}
	}
	if (mm_private_clone(newmm, oldmm) < 0)
		goto fail;

	/* Logical slots own the child's contents. Child translations are
	 * demand populated, so fork needs no child leaf/page-table copies.
	 * No fallible operation follows the parent's COW transition. */
	for_each_vma (vma, oldmm) {
		if (vma->vm_shared)
			continue;
		for (uintptr_t va = vma->vm_start; va < vma->vm_end;
		     va += PAGE_SIZE) {
			pte_t *pte = pt_lookup(oldmm->pgroot, va);
			pte_t entry;

			if (!pte || !pte_upage(*pte))
				continue;
			struct mm_page_slot *slot = mm_private_find(oldmm, va);
			if (slot && !slot->cow)
				continue;
			entry = mm_private_child_pte(*pte);
			if (entry != *pte) {
				*pte = entry;
				parent_downgraded = true;
			}
		}
	}
	if (parent_downgraded) {
		tlb_flush_all();
		mm_flush_remote(oldmm, false);
	}
	mm_unlock(oldmm);
	return newmm;
fail:
	mm_unlock(oldmm);
	mm_put(newmm);
	return NULL;
}

pte_t *create_pgroot(struct mm_struct *mm)
{
	if (!mm || mm_lifecycle_state(mm) != MM_LIFECYCLE_BUILDING)
		return NULL;
	return pgtable_create();
}

static void mm_finish_retirement(struct mm_struct *mm)
{
	pte_t *pgd;

	mm_begin_retirement(mm);
	destroy_mappings(mm);
	mm_private_remove(mm, 0, TASK_SIZE);
	vma_discard_spares(mm);
	pgd = mm->pgroot;
	mm->pgroot = NULL;
	if (pgd)
		pgtable_destroy(pgd);
	kfree(mm);
}

__cold
void mm_destroy(struct mm_struct *mm)
{
	if (!mm)
		return;
	mm_finish_retirement(mm);
}

void mm_reap_retired(void)
{
	for (;;) {
		struct mm_struct *mm;
		irq_flags_t flags;

		spin_lock_irqsave(&mm_retired_lock, flags);
		if (list_empty(&mm_retired)) {
			spin_unlock_irqrestore(&mm_retired_lock, flags);
			return;
		}
		mm = list_first_entry(&mm_retired, struct mm_struct,
				      retirement);
		list_del(&mm->retirement);
		spin_unlock_irqrestore(&mm_retired_lock, flags);
		mm_destroy(mm);
	}
}

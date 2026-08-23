/*
 * mm/mmap.c - 用户地址空间管理
 */

#include <nuvix/mm.h>
#include <nuvix/fdtable.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/page_cache.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/buddy.h>
#include <nuvix/task.h>
#include <nuvix/user_map.h>
#include <uapi/mman.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/processor.h>
#include <asm/csr.h>

#include "internal.h"

#define MM_MMAP_COMPAT_FLAGS                                                   \
	(MAP_DENYWRITE | MAP_EXECUTABLE | MAP_NORESERVE | MAP_STACK)
#define MM_MMAP_PLACEMENT_FLAGS (MAP_FIXED | MAP_FIXED_NOREPLACE)
#define MM_MMAP_SUPPORTED_FLAGS                                                \
	(MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS | MM_MMAP_COMPAT_FLAGS |     \
	 MM_MMAP_PLACEMENT_FLAGS | MAP_POPULATE)

__always_inline __must_check __pure
static inline bool vma_release_dirty(const struct vm_area_struct *vma)
{
	return vma && vma->vm_file && vma->vm_shared &&
	       (vma->vm_flags & VM_WRITE);
}

static uintptr_t find_unmapped_area(struct mm_struct *mm, size_t length)
{
	uintptr_t len;
	uintptr_t low = mm_page_align_up(mm->brk);
	uintptr_t start;

	if (length > TASK_SIZE)
		return 0;

	len = mm_page_align_up(length);
	if (len == 0 || len >= USER_STACK_BASE)
		return 0;

	if (low < PAGE_SIZE)
		low = PAGE_SIZE;

	start = (USER_STACK_BASE - len) & PAGE_MASK;

	while (start >= low) {
		if (!user_map_reserved_overlaps(start, start + len) &&
		    !vma_range_overlaps(mm, start, start + len))
			return start;
		if (start < low + PAGE_SIZE)
			break;
		start -= PAGE_SIZE;
	}

	return 0;
}

static int mmap_parse_flags(int flags, bool *shared, bool *private, bool *fixed,
			    bool *noreplace, bool *populate)
{
	int type = flags & MAP_TYPE;
	bool validate = type == MAP_SHARED_VALIDATE;

	if ((flags & ~MM_MMAP_SUPPORTED_FLAGS) != 0)
		return validate ? -EOPNOTSUPP : -EINVAL;

	*shared = false;
	*private = false;
	switch (type) {
	case MAP_SHARED:
	case MAP_SHARED_VALIDATE:
		*shared = true;
		break;
	case MAP_PRIVATE:
		*private = true;
		break;
	default:
		return -EINVAL;
	}

	*noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;
	*fixed = (flags & MM_MMAP_PLACEMENT_FLAGS) != 0;
	*populate = (flags & MAP_POPULATE) != 0;
	return 0;
}

__always_inline __must_check __pure
static inline int mmap_populate_access(const struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_READ)
		return USER_FAULT_READ;
	if (vma->vm_flags & VM_EXEC)
		return USER_FAULT_EXEC;
	return -EINVAL;
}

static void mmap_populate_range(struct mm_struct *mm, uintptr_t start,
				uintptr_t end)
{
	uintptr_t cursor = start;

	while (cursor < end) {
		struct vm_area_struct *vma;
		uintptr_t segment_end;
		int access;
		int ret;

		mm_lock(mm);
		vma = find_vma(mm, cursor);
		if (!vma) {
			mm_unlock(mm);
			return;
		}

		segment_end = MIN(vma->vm_end, end);
		access = mmap_populate_access(vma);
		mm_unlock(mm);

		if (segment_end <= cursor)
			return;
		if (access >= 0) {
			ret = fault_in_user_range(mm, cursor,
						  segment_end - cursor, access);
			if (ret < 0)
				return;
		}
		cursor = segment_end;
	}
}

static int install_vma_locked(struct mm_struct *mm, uintptr_t start,
			      uintptr_t end, uint32_t vm_flags, uint32_t type,
			      struct file *file, uint64_t file_offset,
			      bool shared, bool get_file)
{
	struct vm_area_struct *vma;

	vma = vma_alloc_slot(mm);
	if (!vma)
		return -ENOMEM;

	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_flags = vm_flags;
	vma->vm_type = type;
	vma->vm_file = file;
	vma->vm_offset = file ? file_offset : 0;
	vma->vm_shared = shared;
	vma->used = true;
	if (file && get_file)
		file_get(file);
	return 0;
}

static struct pgcache *vma_page_cache_get(const struct vm_area_struct *vma,
					     uintptr_t va)
{
	if (!vma || !vma->vm_file || !vma->vm_file->f_inode)
		return NULL;

	return pgcache_get_mapping(&vma->vm_file->f_inode->i_pages,
				      vma_page_index(vma, va), PAGE_CACHE_READ,
				      NULL);
}

static void vma_mark_shared_page_dirty(const struct vm_area_struct *vma,
				       uintptr_t va)
{
	struct pgcache *page;

	if (!vma_release_dirty(vma))
		return;

	page = vma_page_cache_get(vma, va);
	if (!page)
		return;

	pgcache_mark_dirty(page);
	pgcache_put_page(page);
}

__must_check __nonnull(1)
static int mm_map_user_pte_like(pte_t *root, uintptr_t va, paddr_t pa,
			     pte_t old_entry)
{
	pgprot_t perm = pte_leaf_prot(old_entry);
	int ret;
	pte_t *pte;

	ret = map_page(root, va, pa,
		       pte_is_present(old_entry)
			       ? perm
			       : pgprot_user(true, false, false));
	if (ret < 0)
		return ret;

	pte = pgtable_lookup(root, va);
	BUG_ON(!pte);
	*pte = pte_make(pa, perm);
	return 0;
}

static pte_t mm_private_child_pte(pte_t entry)
{
	if (!pte_is_present(entry))
		return entry;

	return pte_make(pte_phys_addr(entry),
			pgprot_make_readonly(pte_leaf_prot(entry)));
}

__cold
static void free_user_page_tables(pte_t *pgd)
{
	for (int i = 0; i < 256; i++) {
		if (!pte_is_present(pgd[i]))
			continue;

		pte_t *pmd = (pte_t *)__va(pte_phys_addr(pgd[i]));
		for (int j = 0; j < 512; j++) {
			if (!pte_is_present(pmd[j]))
				continue;

			pte_t *pt = (pte_t *)__va(pte_phys_addr(pmd[j]));
			for (int k = 0; k < 512; k++)
				pt[k] = 0;
			free_page(pt, 0);
		}
		free_page(pmd, 0);
	}
	free_page(pgd, 0);
}

static int mm_unmap_range_locked(struct mm_struct *mm, uintptr_t addr,
				 uintptr_t end, struct mm_teardown *teardown)
{
	int ret = 0;

	if (vma_munmap_slots_needed(mm, addr, end) > vma_free_slot_count(mm))
		return -ENOMEM;
	ret = mm_teardown_reserve_range(mm, addr, end, teardown);
	if (ret < 0)
		return ret;

	for (int i = 0; i < NR_VMA; i++) {
		uintptr_t unmap_start = 0;
		uintptr_t unmap_end = 0;
		struct vm_area_struct *vma = &mm->vma[i];

		if (!vma_overlaps(vma, addr, end))
			continue;

		unmap_start = addr > vma->vm_start ? addr : vma->vm_start;
		unmap_end = end < vma->vm_end ? end : vma->vm_end;
		mm_unmap_user_pages_locked(mm, vma, unmap_start, unmap_end,
					   teardown);

	}

	mm_teardown_sync(mm, teardown, false);

	/* Slot capacity was checked before touching any PTE, so metadata updates
	 * cannot fail after the translation invalidation has completed. */
	for (int i = 0; i < NR_VMA; i++) {
		struct vm_area_struct *vma = &mm->vma[i];

		if (!vma_overlaps(vma, addr, end))
			continue;
		ret = vma_unmap_range(mm, vma, addr, end, NULL, NULL);
		BUG_ON(ret < 0);
		if (ret > 0)
			ret = 0;
	}
	return ret;
}

static int madvise_dontneed_range(struct mm_struct *mm, uintptr_t start,
				  uintptr_t end, struct mm_teardown *teardown)
{
	uintptr_t cursor = start;
	int ret;

	ret = mm_teardown_reserve_range(mm, start, end, teardown);
	if (ret < 0)
		return ret;

	while (cursor < end) {
		struct vm_area_struct *vma = find_vma(mm, cursor);
		uintptr_t segment_end;

		if (!vma) {
			mm_teardown_sync(mm, teardown, false);
			return -ENOMEM;
		}

		segment_end = MIN(vma->vm_end, end);
		if (segment_end <= cursor) {
			mm_teardown_sync(mm, teardown, false);
			return -ENOMEM;
		}

		mm_unmap_user_pages_locked(mm, vma, cursor, segment_end, teardown);
		cursor = segment_end;
	}

	mm_teardown_sync(mm, teardown, false);
	return 0;
}

__must_check __nonnull(3, 4)
static int mm_mlock_range(uintptr_t addr, size_t len, uintptr_t *start,
		       uintptr_t *end)
{
	uintptr_t raw_end;
	uintptr_t aligned_end;

	if (len == 0) {
		*start = addr;
		*end = addr;
		return 0;
	}

	raw_end = addr + len;
	if (raw_end < addr)
		return -EINVAL;

	aligned_end = ALIGN_UP(raw_end, PAGE_SIZE);
	if (aligned_end < raw_end)
		return -EINVAL;
	if (aligned_end > TASK_SIZE)
		return -ENOMEM;

	*start = ALIGN_DOWN(addr, PAGE_SIZE);
	*end = aligned_end;
	return 0;
}

__must_check __nonnull(1)
static int mm_mlock_validate_range(struct mm_struct *mm, uintptr_t start,
				uintptr_t end)
{
	int ret = 0;

	mm_lock(mm);
	if (!vma_range_is_mapped(mm, start, end))
		ret = -ENOMEM;
	mm_unlock(mm);
	return ret;
}

__must_check __nonnull(1, 2)
static ssize_t mremap_move_locked(struct mm_struct *mm, const struct vm_area_struct *old_vma,
				   uintptr_t old_addr, uintptr_t old_end,
				   size_t old_len, size_t new_len, uintptr_t fixed_addr,
				   bool fixed, struct mm_teardown *teardown)
{
	struct vm_area_struct new_template;
	struct vm_area_struct *new_vma;
	uintptr_t new_start;
	uintptr_t new_end;
	size_t move_len = MIN(old_len, new_len);
	int ret;

	if (fixed) {
		new_start = fixed_addr;
	} else {
		new_start = find_unmapped_area(mm, new_len);
		if (!new_start)
			return -ENOMEM;
	}

	ret = mm_range_end_page_aligned(new_start, new_len, &new_end);
	if (ret < 0)
		return ret;
	if (new_end > USER_STACK_BASE ||
	    user_map_reserved_overlaps(new_start, new_end))
		return -EINVAL;
	if (old_addr < new_end && new_start < old_end)
		return -EINVAL;

	/* Reserve every mapping reference that this move may detach before the
	 * destination is touched.  The inner unmap calls then cannot fail due to
	 * allocation after a partial move has become visible. */
	if (fixed) {
		ret = mm_teardown_reserve_range(mm, new_start, new_end,
						teardown);
		if (ret < 0)
			return ret;
	}
	ret = mm_teardown_reserve_range(mm, old_addr, old_end, teardown);
	if (ret < 0)
		return ret;

	new_template = *old_vma;
	new_template.vm_start = new_start;
	new_template.vm_end = new_end;
	if (new_template.vm_file)
		new_template.vm_offset = vma_offset_at(old_vma, old_addr);

	if (fixed) {
		/* Pre-flight the whole move before tearing down the
		 * destination.  The destination unmap may split VMAs, the
		 * old-range unmap needs `old_needed` split slots, and the
		 * new VMA needs one more; every one of these must fit in
		 * the slots that exist *before* the destination is
		 * unmapped.  Otherwise the -ENOMEM error path would return
		 * after the destination range was already destroyed. */
		if (vma_munmap_slots_needed(mm, new_start, new_end) +
		    vma_munmap_slots_needed(mm, old_addr, old_end) + 1 >
		    vma_free_slot_count(mm))
			return -ENOMEM;
		ret = mm_unmap_range_locked(mm, new_start, new_end, teardown);
		if (ret < 0)
			return ret;
	}

	if (vma_munmap_slots_needed(mm, old_addr, old_end) + 1 >
	    vma_free_slot_count(mm))
		return -ENOMEM;

	new_vma = vma_alloc_slot(mm);
	if (!new_vma)
		return -ENOMEM;

	*new_vma = new_template;
	if (new_vma->vm_file)
		file_get(new_vma->vm_file);

	ret = mm_move_user_pages_locked(mm, old_addr, new_start, move_len);
	if (ret < 0) {
		vma_free_slot(new_vma);
		return ret;
	}

	ret = mm_unmap_range_locked(mm, old_addr, old_end, teardown);
	if (ret < 0) {
		return ret;
	}

	vma_merge_all(mm);
	return (ssize_t)new_start;
}

void mm_pte_mapping_get(paddr_t pa)
{
	struct pgcache *page = pgcache_get_data(__va(pa));
	struct page *buddy_page;

	if (page)
		return;

	buddy_page = virt_to_page(__va(pa));
	if (!buddy_page)
		return;
	page_get(buddy_page);
}

void mm_pte_mapping_put(const struct vm_area_struct *vma, paddr_t pa)
{
	bool dirty = vma_release_dirty(vma);

	mm_pte_mapping_put_dirty(pa, dirty);
}

void mm_pte_mapping_put_dirty(paddr_t pa, bool dirty)
{
	struct pgcache *page = pgcache_get_data(__va(pa));
	struct page *buddy_page;

	if (page) {
		if (dirty)
			pgcache_mark_dirty(page);
		pgcache_put_page(page);
		pgcache_put_page(page);
		return;
	}

	buddy_page = virt_to_page(__va(pa));
	if (buddy_page)
			page_put(buddy_page);
}

int mm_teardown_reserve_range(struct mm_struct *mm, uintptr_t start,
			      uintptr_t end,
			      struct mm_teardown *teardown)
{
	size_t needed = 0;
	size_t capacity;
	struct mm_mapping_release *release;

	if (!mm || !teardown || start > end)
		return -EINVAL;

	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		pte_t *pte = pgtable_lookup(mm->pgd, va);

		if (pte && pte_is_user_page(*pte))
			needed++;
	}
	if (needed == 0)
		return 0;
	if (teardown->nr_release > (size_t)-1 - needed)
		return -ENOMEM;

	capacity = teardown->nr_release + needed;
	if (capacity <= teardown->release_capacity)
		return 0;

	release = kmalloc_array(capacity, sizeof(*release), ALLOC_NOWAIT);
	if (!release)
		return -ENOMEM;
	if (teardown->nr_release)
		memcpy(release, teardown->release,
		       teardown->nr_release * sizeof(*release));
	kfree(teardown->release);
	teardown->release = release;
	teardown->release_capacity = capacity;
	return 0;
}

void mm_unmap_user_pages_locked(struct mm_struct *mm,
				const struct vm_area_struct *vma,
				uintptr_t start, uintptr_t end,
				struct mm_teardown *teardown)
{
	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		pte_t *pte = pgtable_lookup(mm->pgd, va);
		struct mm_mapping_release *release;

		if (!pte || !pte_is_user_page(*pte))
			continue;
		BUG_ON(teardown->nr_release >= teardown->release_capacity);
		release = &teardown->release[teardown->nr_release++];
		release->pa = pte_phys_addr(*pte);
		release->dirty = vma_release_dirty(vma);
		*pte = 0;
	}
}

void mm_teardown_sync(struct mm_struct *mm, struct mm_teardown *teardown,
			      bool flush_icache)
{
	bool changed = teardown && teardown->nr_release != 0;

	if (changed)
		flush_tlb_all();
	if (flush_icache)
		icache_flush();
	if (changed || flush_icache)
		mm_flush_remote(mm, flush_icache);
}

void mm_teardown_release(struct mm_teardown *teardown)
{
	if (!teardown)
		return;
	for (size_t i = 0; i < teardown->nr_release; i++)
		mm_pte_mapping_put_dirty(teardown->release[i].pa,
					 teardown->release[i].dirty);
	kfree(teardown->release);
	memset(teardown, 0, sizeof(*teardown));
}

void mm_replace_user_pte_locked(struct mm_struct *mm,
				const struct vm_area_struct *vma,
				uintptr_t va, pte_t *pte, pte_t new_entry,
				paddr_t old_pa, struct mm_teardown *teardown)
{
	struct mm_mapping_release *release;

	BUG_ON(!teardown);
	BUG_ON(teardown->nr_release >= teardown->release_capacity);
	release = &teardown->release[teardown->nr_release++];
	release->pa = old_pa;
	release->dirty = vma_release_dirty(vma);
	*pte = new_entry;
	flush_tlb_page(va);
	mm_flush_remote(mm, false);
}


/* Reference transfer only: the mapping ref moves from the old PTE to the new
 * one, so this deliberately bypasses mm_pte_mapping_get/put.  Any future change
 * that drops or retains a PTE here must route through mm_pte_mapping_put. */
int mm_move_user_pages_locked(struct mm_struct *mm, uintptr_t old_start,
			      uintptr_t new_start, size_t len)
{
	uintptr_t old_end;
	uintptr_t new_end;

	if (!mm)
		return -EINVAL;
	if (len == 0)
		return 0;
	if ((old_start | new_start) & (PAGE_SIZE - 1))
		return -EINVAL;
	if (mm_range_end_page_aligned(old_start, len, &old_end) < 0 ||
	    mm_range_end_page_aligned(new_start, len, &new_end) < 0)
		return -EINVAL;
	if (old_start < new_end && new_start < old_end)
		return -EINVAL;

	for (uintptr_t va = new_start; va < new_end; va += PAGE_SIZE) {
		pte_t *pte = pgtable_lookup(mm->pgd, va);

		if (pte && pte_is_user_page(*pte))
			return -EEXIST;
	}

	uintptr_t mapped_end = new_start;

	for (uintptr_t old_va = old_start, new_va = new_start; old_va < old_end;
	     old_va += PAGE_SIZE, new_va += PAGE_SIZE) {
		pte_t *old_pte = pgtable_lookup(mm->pgd, old_va);
		pte_t old_entry;
		int ret;

		if (!old_pte || !pte_is_user_page(*old_pte))
			continue;

		old_entry = *old_pte;
		ret = mm_map_user_pte_like(mm->pgd, new_va,
					   pte_phys_addr(old_entry), old_entry);
		if (ret < 0) {
			for (uintptr_t va = new_start; va < mapped_end;
			     va += PAGE_SIZE) {
				pte_t *pte = pgtable_lookup(mm->pgd, va);

				if (pte)
					*pte = 0;
			}
			return ret;
		}
		mapped_end = new_va + PAGE_SIZE;
	}

	for (uintptr_t old_va = old_start; old_va < old_end;
	     old_va += PAGE_SIZE) {
		pte_t *old_pte = pgtable_lookup(mm->pgd, old_va);

		if (!old_pte || !pte_is_user_page(*old_pte))
			continue;
		*old_pte = 0;
	}

	flush_tlb_all();
	mm_flush_remote(mm, false);
	return 0;
}

struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm =
		kmalloc(sizeof(struct mm_struct), ALLOC_NOWAIT);
	if (!mm)
		return NULL;

	memset(mm, 0, sizeof(struct mm_struct));
	refcount_set(&mm->refcount, 1);
	mutex_init(&mm->mmap_lock, LOCK_RANK_MM_MMAP, LOCK_IRQ_TASK_ONLY);
	return mm;
}

struct mm_struct *mm_create_user(void)
{
	struct mm_struct *mm = mm_alloc();

	if (!mm)
		return NULL;

	mm->pgd = mm_create_user_pgd();
	if (!mm->pgd) {
		mm_destroy(mm);
		return NULL;
	}

	return mm;
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

	if (refcount_dec_and_test(&mm->refcount))
		mm_destroy(mm);
}

int mm_refcount_read(const struct mm_struct *mm)
{
	return refcount_read(&mm->refcount);
}

void mm_membarrier_register(struct mm_struct *mm, uint32_t cmd)
{
	BUG_ON(!mm);
	mm->membarrier_registrations |= cmd;
}

uint32_t mm_membarrier_registrations(const struct mm_struct *mm)
{
	BUG_ON(!mm);
	return mm->membarrier_registrations;
}

uintptr_t mm_pgroot(const struct mm_struct *mm)
{
	BUG_ON(!mm || !mm->pgd);
	return pgtable_make_token(mm->pgd);
}

struct mm_struct *dup_mm(struct mm_struct *oldmm)
{
	struct mm_struct *newmm;
	struct vm_area_struct *failed_vma = NULL;
	paddr_t failed_pa = 0;
	bool failed_mapping_ref = false;
	bool parent_downgraded = false;

	if (!oldmm)
		return NULL;

	newmm = mm_alloc();
	if (!newmm)
		return NULL;

	newmm->pgd = mm_create_user_pgd();
	if (!newmm->pgd) {
		kfree(newmm);
		return NULL;
	}

	mm_lock(oldmm);
	newmm->brk = oldmm->brk;
	newmm->code_start = oldmm->code_start;
	newmm->code_end = oldmm->code_end;
	newmm->membarrier_registrations = oldmm->membarrier_registrations;
	memcpy(newmm->vma, oldmm->vma, sizeof(oldmm->vma));
	for (int i = 0; i < NR_VMA; i++) {
		if (newmm->vma[i].used && newmm->vma[i].vm_file)
			file_get(newmm->vma[i].vm_file);
	}

	for (int i = 0; i < NR_VMA; i++) {
		struct vm_area_struct *old_vma = &oldmm->vma[i];
		struct vm_area_struct *new_vma = &newmm->vma[i];

		if (!old_vma->used ||
		    (old_vma->vm_file && old_vma->vm_shared))
			continue;

		uintptr_t start = old_vma->vm_start;
		uintptr_t end = old_vma->vm_end;

		for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
			pte_t *pte = pgtable_lookup(oldmm->pgd, va);
			pte_t entry;
			pte_t child_entry;
			paddr_t pa;
			int ret;

			if (!pte || !pte_is_user_page(*pte))
				continue;

			entry = *pte;
			pa = pte_phys_addr(entry);
			child_entry = old_vma->vm_shared
					      ? entry
					      : mm_private_child_pte(entry);
			/* The acquired reference becomes the child PTE's
			 * reference only after its exact entry is installed. */
			mm_pte_mapping_get(pa);
			ret = mm_map_user_pte_like(newmm->pgd, va, pa, child_entry);
			if (ret < 0) {
				failed_vma = new_vma;
				failed_pa = pa;
				failed_mapping_ref = true;
				goto fail_loop;
			}
		}
	}

	/* Commit the parent's COW permissions only after every child PTE is
	 * installed, so a failed fork leaves the parent untouched. */
	for (int i = 0; i < NR_VMA; i++) {
		struct vm_area_struct *vma = &oldmm->vma[i];

		if (!vma->used || vma->vm_shared)
			continue;

		for (uintptr_t va = vma->vm_start; va < vma->vm_end;
		     va += PAGE_SIZE) {
			pte_t *pte = pgtable_lookup(oldmm->pgd, va);
			pte_t cow_entry;

			if (!pte || !pte_is_user_page(*pte) ||
			    !pte_is_present(*pte))
				continue;

			cow_entry = mm_private_child_pte(*pte);
			if (*pte != cow_entry) {
				*pte = cow_entry;
				parent_downgraded = true;
			}
		}
	}
	if (parent_downgraded) {
		flush_tlb_all();
		mm_flush_remote(oldmm, false);
	}
	mm_unlock(oldmm);

	return newmm;

fail_loop:
	mm_unlock(oldmm);
	if (failed_mapping_ref)
		mm_pte_mapping_put(failed_vma, failed_pa);
	mm_destroy(newmm);
	return NULL;
}

void mm_destroy(struct mm_struct *mm)
{
	struct mm_teardown teardown = {0};
	int ret;

	if (!mm)
		return;

	if (mm->pgd) {
		mm_lock(mm);
		ret = mm_unmap_range_locked(mm, 0, TASK_SIZE, &teardown);
		BUG_ON(ret < 0);
		mm_unlock(mm);
		mm_teardown_release(&teardown);
		free_user_page_tables(mm->pgd);
	}

	kfree(mm);
}

pte_t *mm_create_user_pgd(void)
{
	pte_t *user_pgd;
	pte_t *kern_root;
	int ret;

	user_pgd = (pte_t *)get_free_page(0, ALLOC_NOWAIT);
	if (!user_pgd)
		return NULL;
	memset(user_pgd, 0, PAGE_SIZE);

	kern_root = current_pt();
	for (int i = 256; i < 512; i++)
		user_pgd[i] = kern_root[i];

	ret = user_map_apply(user_pgd);
	if (ret < 0) {
		free_user_page_tables(user_pgd);
		return NULL;
	}

	return user_pgd;
}

struct vm_area_struct *find_vma(struct mm_struct *mm, uintptr_t addr)
{
	for (int i = 0; i < NR_VMA; i++) {
		if (mm->vma[i].used && addr >= mm->vma[i].vm_start &&
		    addr < mm->vma[i].vm_end)
			return &mm->vma[i];
	}
	return NULL;
}

int mm_mapping_identity_get(struct mm_struct *mm, uintptr_t addr,
			    struct mm_mapping_identity *identity)
{
	struct vm_area_struct *vma;
	int ret = 0;

	if (!mm)
		return -EFAULT;

	memset(identity, 0, sizeof(*identity));
	mm_lock(mm);
	vma = find_vma(mm, addr);
	if (!vma) {
		ret = -EFAULT;
		goto out;
	}
	if (!vma->vm_shared) {
		identity->kind = MM_MAPPING_PRIVATE;
		goto out;
	}
	if (!vma->vm_file) {
		identity->kind = MM_MAPPING_SHARED_ANON;
		goto out;
	}
	if (!vma->vm_file->f_inode) {
		ret = -EFAULT;
		goto out;
	}

	identity->kind = MM_MAPPING_SHARED_FILE;
	identity->mapping = &vma->vm_file->f_inode->i_pages;
	identity->pgoff = vma_page_index(vma, addr);
	identity->file = vma->vm_file;
	file_get(identity->file);

out:
	mm_unlock(mm);
	return ret;
}

void mm_mapping_identity_put(struct mm_mapping_identity *identity)
{
	if (!identity)
		return;

	file_put(identity->file);
	memset(identity, 0, sizeof(*identity));
}

int mm_user_page_resident(struct mm_struct *mm, uintptr_t addr, bool *resident)
{
	pte_t *pte;

	if (!mm || !resident)
		return -EINVAL;

	mm_lock(mm);
	if (!find_vma(mm, addr)) {
		mm_unlock(mm);
		return -ENOMEM;
	}

	pte = pgtable_lookup(mm->pgd, addr);
	*resident = pte && pte_is_user_page(*pte);
	mm_unlock(mm);
	return 0;
}

uintptr_t mm_brk(struct mm_struct *mm, uintptr_t addr)
{
	uintptr_t ret;
	uintptr_t old_brk;
	struct vm_area_struct *heap_vma = NULL;

	if (!mm)
		return 0;

	mm_lock(mm);
	old_brk = mm->brk;
	ret = old_brk;

	if (addr == 0)
		goto out;

	if (addr <= old_brk)
		goto out;
	if (addr > TASK_SIZE)
		goto out;

	for (int i = 0; i < NR_VMA; i++) {
		if (mm->vma[i].used && mm->vma[i].vm_type == VMA_HEAP) {
			heap_vma = &mm->vma[i];
			break;
		}
	}

	if (!heap_vma) {
		if (vma_range_overlaps(mm, old_brk, addr))
			goto out;

		heap_vma = vma_alloc_slot(mm);
		if (!heap_vma)
			goto out;
		heap_vma->vm_start = old_brk;
		heap_vma->vm_end = addr;
		heap_vma->vm_flags = VM_READ | VM_WRITE;
		heap_vma->vm_type = VMA_HEAP;
		heap_vma->used = true;
	} else {
		if (vma_range_overlaps_other(mm, heap_vma, heap_vma->vm_start,
					     addr))
			goto out;

		heap_vma->vm_end = addr;
	}

	mm->brk = addr;
	vma_merge_all(mm);
	ret = addr;

out:
	mm_unlock(mm);
	return ret;
}

ssize_t mm_mmap_file(struct mm_struct *mm, uintptr_t addr, size_t length,
		     int prot, int flags, int fd, uint64_t offset)
{
	uintptr_t start;
	uintptr_t end;
	uint32_t vm_flags = 0;
	struct file *file = NULL;
	bool anonymous;
	bool shared;
	bool private;
	bool fixed;
	bool noreplace;
	bool populate;
	struct mm_teardown teardown = {0};
	ssize_t ret;

	if (!mm)
		return -ENOMEM;

	if (!mm_prot_is_valid(prot))
		return -EINVAL;

	ret = mmap_parse_flags(flags, &shared, &private, &fixed, &noreplace,
			       &populate);
	if (ret < 0)
		return ret;

	anonymous = (flags & MAP_ANONYMOUS) != 0;
	if (shared == private)
		return -EINVAL;

	if (fixed) {
		if (addr == 0 || (addr & (PAGE_SIZE - 1)))
			return -EINVAL;
	}

	if (!anonymous && (offset & (PAGE_SIZE - 1)))
		return -EINVAL;

	vm_flags = mm_prot_to_vm_flags(prot);

	if (length == 0 || length > TASK_SIZE)
		return -EINVAL;

	if (!anonymous) {
		file = fd_get(fd);
		if (!file)
			return -EBADF;
		if (!file->f_inode || !S_ISREG(file->f_inode->i_mode)) {
			ret = -EINVAL;
			goto put_file;
		}
		if (!(file->f_mode & FMODE_READ)) {
			ret = -EACCES;
			goto put_file;
		}
		if (shared && (vm_flags & VM_WRITE) &&
		    !(file->f_mode & FMODE_WRITE)) {
			ret = -EACCES;
			goto put_file;
		}
	}

	mm_lock(mm);

	if (fixed) {
		start = addr;
	} else if (addr != 0) {
		start = mm_page_align_up(addr);
	} else {
		start = find_unmapped_area(mm, length);
		if (!start) {
			ret = -ENOMEM;
			goto out;
		}
	}

	ret = mm_range_end_page_aligned(start, length, &end);
	if (ret < 0)
		goto out;

	if (end > USER_STACK_BASE) {
		ret = -EINVAL;
		goto out;
	}

	if (user_map_reserved_overlaps(start, end)) {
		ret = -EINVAL;
		goto out;
	}

	if (fixed) {
		if (noreplace && vma_range_overlaps(mm, start, end)) {
			ret = -EEXIST;
			goto out;
		}
		if (!noreplace) {
			ret = mm_unmap_range_locked(mm, start, end, &teardown);
			if (ret < 0)
				goto out;
		}
	} else if (vma_range_overlaps(mm, start, end)) {
		ret = -EINVAL;
		goto out;
	}

	ret = install_vma_locked(mm, start, end, vm_flags, VMA_MMAP, file,
				 anonymous ? 0 : offset, shared, false);
	if (ret < 0)
		goto out;
	file = NULL;
	vma_merge_all(mm);
	ret = start;

out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	if (ret >= 0 && populate)
		mmap_populate_range(mm, (uintptr_t)ret, end);
put_file:
	file_put(file);
	return ret;
}

ssize_t mm_mmap(struct mm_struct *mm, uintptr_t addr, size_t length, int prot,
		int flags)
{
	return mm_mmap_file(mm, addr, length, prot, flags, -1, 0);
}

int mm_munmap(struct mm_struct *mm, uintptr_t addr, size_t length)
{
	struct mm_teardown teardown = {0};
	uintptr_t end;
	int ret = 0;

	if (!mm)
		return -ENOMEM;

	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;

	ret = mm_range_end_page_aligned(addr, length, &end);
	if (ret < 0)
		return ret;

	mm_lock(mm);
	ret = mm_unmap_range_locked(mm, addr, end, &teardown);
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

int mm_madvise(struct mm_struct *mm, uintptr_t addr, size_t len, int advice)
{
	uintptr_t end;
	struct mm_teardown teardown = {0};
	int ret = 0;
	bool drop_resident = false;

	if (!mm)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (len == 0)
		return 0;

	switch (advice) {
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
	case MADV_WILLNEED:
	case MADV_FREE:
		break;
	case MADV_DONTNEED:
		drop_resident = true;
		break;
	default:
		return -EINVAL;
	}

	ret = mm_range_end_page_aligned(addr, len, &end);
	if (ret < 0)
		return ret;

	mm_lock(mm);

	for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
		struct vm_area_struct *vma = find_vma(mm, va);

		if (!vma) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (drop_resident)
		ret = madvise_dontneed_range(mm, addr, end, &teardown);

out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

int mm_mlock(struct mm_struct *mm, uintptr_t addr, size_t len)
{
	uintptr_t start;
	uintptr_t end;
	uintptr_t cursor;
	int ret;

	if (len == 0)
		return 0;

	ret = mm_mlock_range(addr, len, &start, &end);
	if (ret < 0)
		return ret;

	ret = mm_mlock_validate_range(mm, start, end);
	if (ret < 0)
		return ret;

	cursor = start;
	while (cursor < end) {
		struct vm_area_struct *vma;
		uintptr_t segment_end;
		bool readable;

		mm_lock(mm);
		vma = find_vma(mm, cursor);
		if (!vma) {
			mm_unlock(mm);
			return -ENOMEM;
		}

		segment_end = MIN(vma->vm_end, end);
		readable = (vma->vm_flags & VM_READ) != 0;
		mm_unlock(mm);

		if (segment_end <= cursor)
			return -ENOMEM;
		if (readable) {
			ret = fault_in_user_range(mm, cursor,
						  segment_end - cursor,
						  USER_FAULT_READ);
			if (ret < 0)
				return ret;
		}
		cursor = segment_end;
	}

	return 0;
}

int mm_munlock(struct mm_struct *mm, uintptr_t addr, size_t len)
{
	uintptr_t start;
	uintptr_t end;
	int ret;

	if (len == 0)
		return 0;

	ret = mm_mlock_range(addr, len, &start, &end);
	if (ret < 0)
		return ret;

	return mm_mlock_validate_range(mm, start, end);
}

ssize_t mm_mremap(struct mm_struct *mm, uintptr_t old_addr, size_t old_size,
		  size_t new_size, int flags, uintptr_t new_addr)
{
	uintptr_t old_end;
	uintptr_t new_end;
	size_t old_len;
	size_t new_len;
	struct mm_teardown teardown = {0};
	struct vm_area_struct *vma;
	int ret;

	if (!mm)
		return -EINVAL;
	if (old_addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (old_size == 0 || new_size == 0)
		return -EINVAL;
	if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
		return -EINVAL;
	if (flags & MREMAP_DONTUNMAP)
		return -EINVAL;
	if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
		return -EINVAL;
	if ((flags & MREMAP_FIXED) &&
	    (new_addr == 0 || (new_addr & (PAGE_SIZE - 1))))
		return -EINVAL;

	ret = mm_range_end_page_aligned(old_addr, old_size, &old_end);
	if (ret < 0)
		return ret;
	old_len = old_end - old_addr;
	ret = mm_range_end_page_aligned(old_addr, new_size, &new_end);
	if (ret < 0)
		return ret;
	new_len = new_end - old_addr;

	mm_lock(mm);
	vma = find_vma(mm, old_addr);
	if (!vma_covers_range(vma, old_addr, old_end)) {
		ret = -EFAULT;
		goto out;
	}
	if (vma->vm_type != VMA_MMAP) {
		ret = -EINVAL;
		goto out;
	}

	if (flags & MREMAP_FIXED) {
		ret = mremap_move_locked(mm, vma, old_addr, old_end, old_len,
					 new_len, new_addr, true, &teardown);
		goto out;
	}

	if (new_len == old_len) {
		ret = (ssize_t)old_addr;
		goto out;
	}

	if (new_len < old_len) {
		ret = mm_unmap_range_locked(mm, new_end, old_end, &teardown);
		if (ret == 0)
			ret = (ssize_t)old_addr;
		goto out;
	}

	if (old_end != vma->vm_end) {
		if (!(flags & MREMAP_MAYMOVE)) {
			ret = -ENOMEM;
			goto out;
		}
		ret = mremap_move_locked(mm, vma, old_addr, old_end, old_len,
					 new_len, 0, false, &teardown);
		goto out;
	}
	if (new_end > USER_STACK_BASE ||
	    user_map_reserved_overlaps(old_end, new_end) ||
	    vma_range_overlaps_other(mm, vma, old_end, new_end)) {
		if (!(flags & MREMAP_MAYMOVE)) {
			ret = -ENOMEM;
			goto out;
		}
		ret = mremap_move_locked(mm, vma, old_addr, old_end, old_len,
					 new_len, 0, false, &teardown);
		goto out;
	}

	vma->vm_end = new_end;
	vma_merge_all(mm);
	ret = (ssize_t)old_addr;

out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

int mm_msync(struct mm_struct *mm, uintptr_t addr, size_t len, int flags)
{
	struct file *sync_files[NR_VMA];
	uintptr_t end;
	size_t nr_sync = 0;
	int ret = 0;

	if (!mm)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		return -EINVAL;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		return -EINVAL;
	if (len == 0)
		return 0;

	ret = mm_range_end_page_aligned(addr, len, &end);
	if (ret < 0)
		return ret;

	mm_lock(mm);
	for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
		struct vm_area_struct *vma = find_vma(mm, va);
		pte_t *pte;
		bool seen;

		if (!vma) {
			ret = -ENOMEM;
			goto out;
		}
		if (!vma_is_anonymous(vma) && !vma->vm_file) {
			ret = -EINVAL;
			goto out;
		}
		if (!vma->vm_file || !vma->vm_shared)
			continue;

		pte = pgtable_lookup(mm->pgd, va);
		if (pte && pte_is_user_page(*pte))
			vma_mark_shared_page_dirty(vma, va);

		if (!(flags & MS_SYNC))
			continue;

		seen = false;
		for (size_t i = 0; i < nr_sync; i++) {
			if (sync_files[i] == vma->vm_file) {
				seen = true;
				break;
			}
		}
		if (!seen && nr_sync < NR_VMA) {
			sync_files[nr_sync] = vma->vm_file;
			file_get(sync_files[nr_sync]);
			nr_sync++;
		}
	}

out:
	mm_unlock(mm);
	for (size_t i = 0; i < nr_sync; i++) {
		if (ret == 0)
			ret = vfs_sync_file(sync_files[i]);
		file_put(sync_files[i]);
	}
	return ret;
}

int mm_map_page(struct mm_struct *mm, uintptr_t va, void *page, int prot)
{
	pte_t *pte;

	if (!mm || !page)
		return -EINVAL;
	if (va & (PAGE_SIZE - 1))
		return -EINVAL;
	if (!mm_prot_is_valid(prot))
		return -EINVAL;

	mm_lock(mm);
	pte = pgtable_lookup(mm->pgd, va);
	if (pte && pte_is_user_page(*pte)) {
		mm_unlock(mm);
		return -EEXIST;
	}
	int ret = map_page(mm->pgd, va, __pa((uintptr_t)page),
			   mm_prot_to_pte_flags(prot));
	mm_unlock(mm);
	return ret;
}

int mm_map_segment(struct mm_struct *mm, uintptr_t start, uintptr_t end,
		   int prot)
{
	int ret = 0;

	if (!mm)
		return -EINVAL;
	if (start >= end || end > USER_STACK_BASE)
		return -EINVAL;
	if (!mm_prot_is_valid(prot))
		return -EINVAL;

	mm_lock(mm);
	if (vma_range_overlaps(mm, start, end)) {
		ret = -EINVAL;
		goto out;
	}

	ret = install_vma_locked(mm, start, end, mm_prot_to_vm_flags(prot),
				 VMA_CODE, NULL, 0, false, false);
	if (ret == -ENOMEM)
		ret = -E2BIG;
out:
	mm_unlock(mm);
	return ret;
}

int mm_map_file_segment(struct mm_struct *mm, struct file *file,
			uintptr_t start, uintptr_t end, int prot,
			uint64_t file_offset)
{
	uint64_t page_delta;
	int ret = 0;

	if (!mm || !file)
		return -EINVAL;
	if (start >= end || end > USER_STACK_BASE)
		return -EINVAL;
	if (!mm_prot_is_valid(prot))
		return -EINVAL;
	if (!file->f_inode || !S_ISREG(file->f_inode->i_mode))
		return -EINVAL;
	if (!(file->f_mode & FMODE_READ))
		return -EACCES;
	if (prot & PROT_WRITE)
		return -EINVAL;

	page_delta = start - (start & PAGE_MASK);
	if ((file_offset & (PAGE_SIZE - 1)) != page_delta)
		return -EINVAL;

	mm_lock(mm);
	if (user_map_reserved_overlaps(start, end) ||
	    vma_range_overlaps(mm, start, end)) {
		ret = -EINVAL;
		goto out;
	}

	ret = install_vma_locked(mm, start, end, mm_prot_to_vm_flags(prot),
				 VMA_CODE, file, file_offset, false, true);
	if (ret == -ENOMEM)
		ret = -E2BIG;
out:
	mm_unlock(mm);
	return ret;
}

int mm_add_stack(struct mm_struct *mm, const void *stack, size_t stack_size)
{
	struct mm_teardown teardown = {0};
	struct vm_area_struct *vma;
	uintptr_t stack_start;
	size_t mapped_size = 0;
	int ret = 0;

	if (!mm || !stack || stack_size != USER_STACK_SIZE)
		return -EINVAL;

	mm_lock(mm);
	if (vma_range_overlaps(mm, USER_STACK_BASE, USER_STACK_TOP)) {
		ret = -EINVAL;
		goto out;
	}

	vma = vma_alloc_slot(mm);
	if (!vma) {
		ret = -E2BIG;
		goto out;
	}

	stack_start = USER_STACK_TOP - stack_size;
	vma->vm_start = stack_start;
	vma->vm_end = USER_STACK_TOP;
	vma->vm_flags = VM_READ | VM_WRITE;
	vma->vm_type = VMA_STACK;
	vma->used = true;
	teardown.release = kmalloc_array(stack_size / PAGE_SIZE,
					 sizeof(*teardown.release), ALLOC_NOWAIT);
	if (!teardown.release) {
		vma_free_slot(vma);
		ret = -ENOMEM;
		goto out;
	}
	teardown.release_capacity = stack_size / PAGE_SIZE;
	for (size_t offset = 0; offset < stack_size; offset += PAGE_SIZE) {
		void *page = get_free_page(0, ALLOC_NOWAIT);

		if (!page) {
			ret = -ENOMEM;
			break;
		}
		memcpy(page, (const uint8_t *)stack + offset, PAGE_SIZE);
		ret = map_page(mm->pgd, stack_start + offset,
			       __pa((uintptr_t)page),
			       pgprot_user(true, true, false));
		if (ret < 0) {
			free_page(page, 0);
			break;
		}
		mapped_size += PAGE_SIZE;
	}
	if (ret < 0) {
		mm_unmap_user_pages_locked(mm, vma, stack_start,
					   stack_start + mapped_size, &teardown);
		mm_teardown_sync(mm, &teardown, false);
		vma_free_slot(vma);
	}
out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

int mm_finalize(struct mm_struct *mm, uintptr_t first_vaddr, uintptr_t last_end)
{
	if (!mm || first_vaddr >= last_end || last_end > USER_STACK_BASE)
		return -EINVAL;

	mm_lock(mm);
	mm->code_start = first_vaddr;
	mm->code_end = PFN_UP(last_end) << PAGE_SHIFT;
	mm->brk = mm->code_end;
	mm_unlock(mm);
	return 0;
}

int mm_mprotect(struct mm_struct *mm, uintptr_t addr, size_t len, int prot)
{
	struct mm_teardown teardown = {0};
	uint32_t new_vm_flags;
	pgprot_t new_pte_flags;
	uintptr_t end;
	int ret = 0;

	if (!mm)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (!mm_prot_is_valid(prot))
		return -EINVAL;
	if (len == 0)
		return 0;
	ret = mm_range_end_page_aligned(addr, len, &end);
	if (ret < 0)
		return ret;

	new_vm_flags = mm_prot_to_vm_flags(prot);
	new_pte_flags = mm_prot_to_pte_flags(prot);

	mm_lock(mm);

	if (!vma_range_is_mapped(mm, addr, end)) {
		ret = -ENOMEM;
		goto out;
	}

	if (vma_mprotect_slots_needed(mm, addr, end) >
	    vma_free_slot_count(mm)) {
		ret = -ENOMEM;
		goto out;
	}

	if (new_vm_flags & VM_WRITE) {
		for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
			struct vm_area_struct *vma = find_vma(mm, va);

			if (vma && vma->vm_file && vma->vm_shared &&
			    !(vma->vm_file->f_mode & FMODE_WRITE)) {
				ret = -EACCES;
				goto out;
			}
		}
	}
	if (prot == PROT_NONE) {
		ret = mm_teardown_reserve_range(mm, addr, end, &teardown);
		if (ret < 0)
			goto out;
	}

	ret = vma_split_range(mm, addr, end);
	if (ret < 0)
		goto out;

	if (prot == PROT_NONE) {
		uintptr_t cursor = addr;

		while (cursor < end) {
			struct vm_area_struct *vma = find_vma(mm, cursor);
			uintptr_t segment_end;

			BUG_ON(!vma || vma->vm_start > cursor);
			segment_end = MIN(vma->vm_end, end);
			BUG_ON(segment_end <= cursor);
			mm_unmap_user_pages_locked(mm, vma, cursor, segment_end,
						   &teardown);
			cursor = segment_end;
		}
	} else {
		for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
			pte_t *pte = pgtable_lookup(mm->pgd, va);

			if (!pte || !pte_is_user_page(*pte))
				continue;
			struct vm_area_struct *vma = find_vma(mm, va);
			uintptr_t pa = pte_phys_addr(*pte);
			pgprot_t pte_flags = new_pte_flags;

			if ((new_vm_flags & VM_WRITE) && vma && !vma->vm_shared) {
				struct pgcache *cache_page = NULL;
				struct page *page;

				if (vma->vm_file)
					cache_page = pgcache_get_data(__va(pa));
				if (cache_page) {
					/* MAP_PRIVATE must never make a
					 * page-cache-backed page writable;
					 * the next write fault COWs it. */
					pte_flags = pgprot_make_readonly(
						new_pte_flags);
					pgcache_put_page(cache_page);
				} else {
					/* A fork-shared mm-owned page must
					 * not become writable in one mm only;
					 * the next write fault splits it. */
					page = virt_to_page(__va(pa));
					if (page &&
					    refcount_read(&page->refcount) > 1)
						pte_flags = pgprot_make_readonly(
							new_pte_flags);
				}
			}
			*pte = pte_make(pa, pte_flags);
		}
	}

	if (prot == PROT_NONE) {
		mm_teardown_sync(mm, &teardown, false);
	} else {
		flush_tlb_all();
		if (prot & PROT_EXEC)
			icache_flush();
		mm_flush_remote(mm, (prot & PROT_EXEC) != 0);
	}
	vma_update_flags_range(mm, addr, end, new_vm_flags);
	vma_merge_all(mm);

out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

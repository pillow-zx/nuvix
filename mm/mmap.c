/*
 * mm/mmap.c - 用户地址空间管理
 */

#include <nuvix/mm.h>
#include <nuvix/math.h>
#include <nuvix/fdtable.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/page_cache.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/buddy.h>
#include <nuvix/task.h>
#include <uapi/mman.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/processor.h>

#include "internal.h"

#define MM_MMAP_COMPAT_FLAGS                                                   \
	(MAP_DENYWRITE | MAP_EXECUTABLE | MAP_NORESERVE | MAP_STACK)
#define MM_MMAP_PLACEMENT_FLAGS (MAP_FIXED | MAP_FIXED_NOREPLACE)
#define MM_MMAP_SUPPORTED_FLAGS                                                \
	(MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS | MM_MMAP_COMPAT_FLAGS |     \
	 MM_MMAP_PLACEMENT_FLAGS | MAP_POPULATE)

static uintptr_t find_unmapped_area(struct mm_struct *mm, size_t length)
{
	uintptr_t len;
	uintptr_t low;
	uintptr_t start;

	if (length > TASK_SIZE)
		return 0;

	len = ALIGN_UP(length, PAGE_SIZE);
	if (len == 0 || len >= USER_STACK_BASE)
		return 0;

	low = ALIGN_UP(mm->brk, PAGE_SIZE);
	if (low < PAGE_SIZE)
		low = PAGE_SIZE;

	start = (USER_STACK_BASE - len) & PAGE_MASK;

	while (start >= low) {
		if (!mm_layout_overlaps(mm, start, start + len) &&
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

__must_check __pure
static inline int
mmap_populate_access(const struct vm_area_struct *vma)
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

	if (shared) {
		vma->vm_anon = anon_shared_create();
		if (!vma->vm_anon) {
			vma_free_slot(vma);
			return -ENOMEM;
		}
	}
	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_flags = vm_flags;
	vma->vm_type = type;
	vma->vm_file = file;
	vma->vm_offset = file ? file_offset : 0;
	vma->vm_shared = shared;
	vma_publish(vma);
	if (file && get_file)
		file_get(file);
	return 0;
}

static int mm_unmap_range_locked(struct mm_struct *mm, uintptr_t addr,
				 uintptr_t end, struct mm_teardown *teardown)
{
	int ret = 0;

	ret = vma_reserve(mm, vma_munmap_slots_needed(mm, addr, end));
	if (ret < 0)
		return ret;
	for_each_vma(vma, mm) {
		if (!vma_overlaps(vma, addr, end))
			continue;
		unmap_pages_locked(mm, vma, MAX(addr, vma->vm_start),
					   MIN(end, vma->vm_end), teardown);
	}

	mm_teardown_sync(mm, teardown, false);
	mm_private_remove(mm, addr, end);

	/* Slot capacity was checked before touching any PTE, so metadata
	 * updates cannot fail after the translation invalidation has completed.
	 */
	for_each_vma(vma, mm) {
		if (!vma_overlaps(vma, addr, end))
			continue;
		ret = vma_unmap_range(mm, vma, addr, end, NULL, NULL);
		BUG_ON(ret < 0);
		if (ret > 0)
			ret = 0;
	}
	return ret;
}

void pte_mapping_get(paddr_t pa)
{
	page_get(virt_to_page(__va(pa)));
}

void pte_mapping_put(paddr_t pa)
{
	page_put(virt_to_page(__va(pa)));
}

void unmap_pages_locked(struct mm_struct *mm,
				const struct vm_area_struct *vma,
				uintptr_t start, uintptr_t end,
				struct mm_teardown *teardown)
{
	(void)vma;
	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		pte_t *pte = pt_lookup(mm->pgroot, va);
		paddr_t *release;

		if (!pte || !pte_upage(*pte))
			continue;
		if (teardown->nr_release == MM_RELEASE_BATCH) {
			mm_teardown_sync(mm, teardown, false);
			mm_teardown_release(teardown);
		}
		release = &teardown->release[teardown->nr_release++];
		*release = PTE_TO_PA(*pte);
		*pte = 0;
	}
}

void mm_teardown_sync(struct mm_struct *mm, struct mm_teardown *teardown,
		      bool flush_icache)
{
	bool changed = teardown && teardown->nr_release != 0;

	if (changed)
		tlb_flush_all();
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
			pte_mapping_put(teardown->release[i]);
	memset(teardown, 0, sizeof(*teardown));
}

void replace_pte_locked(struct mm_struct *mm,
				const struct vm_area_struct *vma, uintptr_t va,
				pte_t *pte, pte_t new_entry, paddr_t old_pa,
				struct mm_teardown *teardown)
{
	paddr_t *release;

	(void)vma;
	BUG_ON(!teardown);
	if (teardown->nr_release == MM_RELEASE_BATCH) {
		mm_teardown_sync(mm, teardown, false);
		mm_teardown_release(teardown);
	}
	release = &teardown->release[teardown->nr_release++];
	*release = old_pa;
	*pte = new_entry;
	tlb_flush_page(va);
	mm_flush_remote(mm, false);
}

void destroy_mappings(struct mm_struct *mm)
{
	struct mm_teardown teardown = {0};
	uintptr_t cursor = 0;
	paddr_t pa;

	mm_lock(mm);
	/* Construction can fail between installing a page and publishing its
	 * VMA. Walk all user leaves, not only the published mapping index. */
	while (mm->pgroot && pgtable_take_upage(mm->pgroot, &cursor, &pa)) {
		teardown.release[teardown.nr_release++] = pa;
		if (teardown.nr_release == MM_RELEASE_BATCH) {
			mm_teardown_sync(mm, &teardown, false);
			mm_teardown_release(&teardown);
		}
	}
	mm_teardown_sync(mm, &teardown, false);
	while (vma_first(mm))
		vma_free_slot(vma_first(mm));
	mm_unlock(mm);
	mm_teardown_release(&teardown);
}

struct vm_area_struct *find_vma(struct mm_struct *mm, uintptr_t addr)
{
	struct rb_node *node = mm->vmas.node;

	while (node) {
		struct vm_area_struct *vma =
			rb_entry(node, struct vm_area_struct, node);

		if (addr < vma->vm_start)
			node = node->left;
		else if (addr >= vma->vm_end)
			node = node->right;
		else
			return vma;
	}
	return NULL;
}

uintptr_t mm_brk(struct mm_struct *mm, uintptr_t addr)
{
	struct mm_teardown teardown = {0};
	uintptr_t old_end, new_end, result;
	int ret;

	if (!mm)
		return 0;
	mm_lock(mm);
	result = mm->brk;
	if (!addr || addr < mm->code_end || addr > USER_STACK_GUARD_BASE)
		goto out;
	old_end = ALIGN_UP(mm->brk, PAGE_SIZE);
	new_end = ALIGN_UP(addr, PAGE_SIZE);
	if (new_end < old_end) {
		ret = mm_unmap_range_locked(mm, new_end, old_end, &teardown);
		if (ret < 0)
			goto out;
	} else if (new_end > old_end) {
		if (mm_layout_overlaps(mm, old_end, new_end) ||
		    vma_range_overlaps(mm, old_end, new_end))
			goto out;
		ret = install_vma_locked(mm, old_end, new_end,
					 VM_READ | VM_WRITE, VMA_HEAP, NULL, 0,
					 false, false);
		if (ret < 0)
			goto out;
	}
	mm->brk = addr;
	result = addr;
	vma_merge_all(mm);
out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return result;
}

ssize_t mm_mmap_file(struct mm_struct *mm, uintptr_t addr, size_t length,
		     int prot, int flags, int fd, uint64_t offset)
{
	uintptr_t start;
	uintptr_t end;
	uint32_t vm_flags = 0;
	struct file *file = NULL;
	struct anon_shared *backing = NULL;
	struct vm_area_struct *prepared = NULL;
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

	if (!mm_root_is_valid(prot))
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
	if (!anonymous && offset > UINT64_MAX - ALIGN_UP(length, PAGE_SIZE))
		return -EOVERFLOW;

	vm_flags = mm_root_to_vm_flags(prot);

	if (length == 0 || length > TASK_SIZE)
		return -EINVAL;

	if (!anonymous) {
		file = fd_get(fd);
		if (!file)
			return -EBADF;
		if (file->f_op && file->f_op->mmap) {
			if (!shared || (prot & PROOT_EXEC)) {
				ret = -EINVAL;
				goto put_file;
			}
			ret = file->f_op->mmap(file, offset,
					     ALIGN_UP(length, PAGE_SIZE), &backing);
			if (ret < 0)
				goto put_file;
			file_put(file);
			file = NULL;
			offset = 0;
			goto have_backing;
		}
		if (shared) {
			ret = -ENODEV;
			goto put_file;
		}
		if (!file->f_inode || !S_ISREG(file->f_inode->i_mode)) {
			ret = -EINVAL;
			goto put_file;
		}
		if (!(file->f_mode & FMODE_READ)) {
			ret = -EACCES;
			goto put_file;
		}
	}

have_backing:
	mm_lock(mm);

	if (fixed) {
		start = addr;
	} else if (addr >= PAGE_SIZE && addr < USER_STACK_BASE &&
		   ALIGN_UP(length, PAGE_SIZE) <=
			   USER_STACK_BASE - (addr & PAGE_MASK) &&
		   !mm_layout_overlaps(mm, addr & PAGE_MASK,
				       (addr & PAGE_MASK) +
					       ALIGN_UP(length, PAGE_SIZE)) &&
		   !vma_range_overlaps(mm, addr & PAGE_MASK,
				       (addr & PAGE_MASK) +
					       ALIGN_UP(length, PAGE_SIZE))) {
		start = addr & PAGE_MASK;
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

	if (mm_layout_overlaps(mm, start, end)) {
		ret = -EINVAL;
		goto out;
	}

	/* All resources for the replacement exist before withdrawing anything.
	 * The prepared VMA owns its backing references, even before
	 * publication. */
	prepared = vma_alloc_slot(mm);
	if (!prepared) {
		ret = -ENOMEM;
		goto out;
	}
	prepared->vm_start = start;
	prepared->vm_end = end;
	prepared->vm_flags = vm_flags;
	prepared->vm_type = VMA_MMAP;
	prepared->vm_offset = anonymous ? 0 : offset;
	prepared->vm_shared = shared;
	prepared->vm_file = file;
	file = NULL;
	if (shared) {
		prepared->vm_anon = backing ? backing : anon_shared_create();
		backing = NULL;
		if (!prepared->vm_anon) {
			ret = -ENOMEM;
			goto out;
		}
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

	vma_publish(prepared);
	prepared = NULL;
	vma_merge_all(mm);
	ret = start;

out:
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	if (ret >= 0 && populate)
		mmap_populate_range(mm, (uintptr_t)ret, end);
put_file:
	anon_shared_put(backing);
	if (prepared)
		vma_free_slot(prepared);
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
	if (mm_layout_overlaps(mm, addr, end))
		ret = -EINVAL;
	else
		ret = mm_unmap_range_locked(mm, addr, end, &teardown);
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

int mm_map_page(struct mm_struct *mm, uintptr_t va, void *page, int prot)
{
	pte_t *pte;

	if (!mm || !page)
		return -EINVAL;
	if (va & (PAGE_SIZE - 1))
		return -EINVAL;
	if (!mm_root_is_valid(prot))
		return -EINVAL;

	mm_lock(mm);
	pte = pt_lookup(mm->pgroot, va);
	if ((pte && pte_upage(*pte)) || mm_private_find(mm, va)) {
		mm_unlock(mm);
		return -EEXIST;
	}
	int ret = mm_private_set(mm, va, virt_to_page(page), false);
	if (ret == 0)
		ret = map_page(mm->pgroot, va, __pa((uintptr_t)page),
			       mm_root_to_pte_flags(prot));
	if (ret < 0)
		mm_private_remove(mm, va, va + PAGE_SIZE);
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
	if (!mm_root_is_valid(prot))
		return -EINVAL;

	start &= PAGE_MASK;
	end = ALIGN_UP(end, PAGE_SIZE);

	mm_lock(mm);
	if (mm_layout_overlaps(mm, start, end) ||
	    vma_range_overlaps(mm, start, end)) {
		ret = -EINVAL;
		goto out;
	}

	ret = install_vma_locked(mm, start, end, mm_root_to_vm_flags(prot),
				 VMA_CODE, NULL, 0, false, false);
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
	if (!mm_root_is_valid(prot))
		return -EINVAL;
	if (!file->f_inode || !S_ISREG(file->f_inode->i_mode))
		return -EINVAL;
	if (!(file->f_mode & FMODE_READ))
		return -EACCES;

	page_delta = start - (start & PAGE_MASK);
	if ((file_offset & (PAGE_SIZE - 1)) != page_delta)
		return -EINVAL;

	start &= PAGE_MASK;
	end = ALIGN_UP(end, PAGE_SIZE);
	file_offset -= page_delta;

	mm_lock(mm);
	if (mm_layout_overlaps(mm, start, end) ||
	    vma_range_overlaps(mm, start, end)) {
		ret = -EINVAL;
		goto out;
	}

	ret = install_vma_locked(mm, start, end, mm_root_to_vm_flags(prot),
				 VMA_CODE, file, file_offset, false, true);
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
	vma_publish(vma);
	for (size_t offset = 0; offset < stack_size; offset += PAGE_SIZE) {
		void *page = get_page(0, ALLOC_NOWAIT);

		if (!page) {
			ret = -ENOMEM;
			break;
		}
		memcpy(page, (const uint8_t *)stack + offset, PAGE_SIZE);
		ret = mm_private_set(mm, stack_start + offset,
				     virt_to_page(page), false);
		if (ret < 0) {
			free_page(page, 0);
			break;
		}
		ret = map_page(mm->pgroot, stack_start + offset,
			       __pa((uintptr_t)page),
			       upgroot(true, true, false));
		if (ret < 0) {
			mm_private_remove(mm, stack_start + offset,
					  stack_start + offset + PAGE_SIZE);
			free_page(page, 0);
			break;
		}
		mapped_size += PAGE_SIZE;
	}
	if (ret < 0) {
		unmap_pages_locked(mm, vma, stack_start,
					   stack_start + mapped_size,
					   &teardown);
		mm_teardown_sync(mm, &teardown, false);
		mm_private_remove(mm, stack_start, stack_start + mapped_size);
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
	uintptr_t end;
	int ret;

	if (!mm || (addr & (PAGE_SIZE - 1)) || !mm_root_is_valid(prot))
		return -EINVAL;
	if (!len)
		return 0;
	ret = mm_range_end_page_aligned(addr, len, &end);
	if (ret < 0)
		return ret;
	mm_lock(mm);
	if (!vma_range_is_mapped(mm, addr, end)) {
		ret = -ENOMEM;
		goto out;
	}
	ret = vma_reserve(mm, vma_mprotect_slots_needed(mm, addr, end));
	if (ret < 0)
		goto out;
	ret = vma_split_range(mm, addr, end);
	if (ret < 0)
		goto out;
	/* PROT_NONE retains a software PTE and its reference. Restoring access
	 * consults the logical COW state, never the physical refcount. */
	for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
		pte_t *pte = pt_lookup(mm->pgroot, va);
		struct vm_area_struct *vma = find_vma(mm, va);
		struct mm_page_slot *slot = mm_private_find(mm, va);
		pgroot_t flags = mm_root_to_pte_flags(prot);

		if (!pte || !pte_upage(*pte))
			continue;
		if (!vma->vm_shared && (!slot || slot->cow))
			flags = pgroot_ro(flags);
		*pte = pte_make(PTE_TO_PA(*pte), flags);
		if (prot == PROOT_NONE)
			pte_clear(pte);
	}
	vma_update_flags_range(mm, addr, end, mm_root_to_vm_flags(prot));
	tlb_flush_all();
	if (prot & PROOT_EXEC)
		icache_flush();
	mm_flush_remote(mm, (prot & PROOT_EXEC) != 0);
	vma_merge_all(mm);
out:
	mm_unlock(mm);
	return ret;
}

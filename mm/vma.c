/*
 * mm/vma.c - 固定数组 VMA 操作
 */

#include "internal.h"

#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/printk.h>
#include <nuvix/page.h>

int mm_range_end_page_aligned(uintptr_t start, size_t length, uintptr_t *end)
{
	uintptr_t aligned_len;

	if (!end)
		return -EINVAL;
	if (length > TASK_SIZE)
		return -EINVAL;

	aligned_len = mm_page_align_up(length);
	if (aligned_len == 0 || start + aligned_len < start)
		return -EINVAL;
	/* Range beyond TASK_SIZE: same contract as mm_mlock_range(). */
	if (start + aligned_len > TASK_SIZE)
		return -ENOMEM;

	*end = start + aligned_len;
	return 0;
}

struct vm_area_struct *vma_alloc_slot(struct mm_struct *mm)
{
	for (int i = 0; i < NR_VMA; i++) {
		if (!mm->vma[i].used)
			return &mm->vma[i];
	}
	return NULL;
}

void vma_free_slot(struct vm_area_struct *vma)
{
	BUG_ON(!vma);
	file_put(vma->vm_file);
	memset(vma, 0, sizeof(*vma));
}

int vma_free_slot_count(struct mm_struct *mm)
{
	int count = 0;

	for (int i = 0; i < NR_VMA; i++) {
		if (!mm->vma[i].used)
			count++;
	}

	return count;
}

bool vma_range_overlaps(struct mm_struct *mm, uintptr_t start, uintptr_t end)
{
	for (int i = 0; i < NR_VMA; i++) {
		if (vma_overlaps(&mm->vma[i], start, end))
			return true;
	}

	return false;
}

bool vma_range_overlaps_other(struct mm_struct *mm,
			      const struct vm_area_struct *skip,
			      uintptr_t start, uintptr_t end)
{
	for (int i = 0; i < NR_VMA; i++) {
		if (&mm->vma[i] == skip)
			continue;
		if (vma_overlaps(&mm->vma[i], start, end))
			return true;
	}

	return false;
}
static bool vma_can_merge(const struct vm_area_struct *a,
			  const struct vm_area_struct *b)
{
	if (!a || !b || a == b)
		return false;
	if (!a->used || !b->used)
		return false;
	if (a->vm_flags != b->vm_flags || a->vm_type != b->vm_type)
		return false;
	if (a->vm_file != b->vm_file || a->vm_shared != b->vm_shared)
		return false;
	if (a->vm_file) {
		if (a->vm_end == b->vm_start)
			return vma_offset_at(a, a->vm_end) == b->vm_offset;
		if (b->vm_end == a->vm_start)
			return vma_offset_at(b, b->vm_end) == a->vm_offset;
		return false;
	}
	return a->vm_end == b->vm_start || b->vm_end == a->vm_start;
}

void vma_merge_all(struct mm_struct *mm)
{
	bool merged;

	do {
		merged = false;
		for (int i = 0; i < NR_VMA && !merged; i++) {
			for (int j = 0; j < NR_VMA; j++) {
				if (i == j ||
				    !vma_can_merge(&mm->vma[i], &mm->vma[j]))
					continue;

				if (mm->vma[i].vm_end == mm->vma[j].vm_start)
					mm->vma[i].vm_end = mm->vma[j].vm_end;
				else
					mm->vma[i].vm_start =
						mm->vma[j].vm_start;
				vma_free_slot(&mm->vma[j]);
				merged = true;
				break;
			}
		}
	} while (merged);
}

int vma_split_at(struct mm_struct *mm, struct vm_area_struct *vma,
		 uintptr_t addr)
{
	struct vm_area_struct *tail;

	if (!vma_contains_split_addr(vma, addr))
		return 0;

	tail = vma_alloc_slot(mm);
	if (!tail)
		return -ENOMEM;

	*tail = *vma;
	if (tail->vm_file) {
		file_get(tail->vm_file);
		tail->vm_offset = vma_offset_at(vma, addr);
	}
	tail->vm_start = addr;
	vma->vm_end = addr;
	return 0;
}

int vma_munmap_slots_needed(struct mm_struct *mm, uintptr_t start,
			    uintptr_t end)
{
	int needed = 0;

	for (int i = 0; i < NR_VMA; i++) {
		struct vm_area_struct *vma = &mm->vma[i];
		uintptr_t unmap_start;
		uintptr_t unmap_end;

		if (!vma_overlaps(vma, start, end))
			continue;

		unmap_start = start > vma->vm_start ? start : vma->vm_start;
		unmap_end = end < vma->vm_end ? end : vma->vm_end;

		if (unmap_start > vma->vm_start && unmap_end < vma->vm_end)
			needed++;
	}

	return needed;
}

int vma_mprotect_slots_needed(struct mm_struct *mm, uintptr_t start,
			      uintptr_t end)
{
	int needed = 0;

	for (int i = 0; i < NR_VMA; i++) {
		struct vm_area_struct *vma = &mm->vma[i];

		if (!vma_overlaps(vma, start, end))
			continue;
		if (vma_contains_split_addr(vma, start))
			needed++;
		if (vma_contains_split_addr(vma, end))
			needed++;
	}

	return needed;
}

bool vma_range_is_mapped(struct mm_struct *mm, uintptr_t start, uintptr_t end)
{
	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		if (!find_vma(mm, va))
			return false;
	}

	return true;
}

int vma_split_range(struct mm_struct *mm, uintptr_t start, uintptr_t end)
{
	int ret;

	for (int i = 0; i < NR_VMA; i++) {
		ret = vma_split_at(mm, &mm->vma[i], start);
		if (ret < 0)
			return ret;
	}

	for (int i = 0; i < NR_VMA; i++) {
		ret = vma_split_at(mm, &mm->vma[i], end);
		if (ret < 0)
			return ret;
	}

	return 0;
}

void vma_update_flags_range(struct mm_struct *mm, uintptr_t start,
			    uintptr_t end, uint32_t vm_flags)
{
	for (int i = 0; i < NR_VMA; i++) {
		struct vm_area_struct *vma = &mm->vma[i];

		if (!vma->used)
			continue;
		if (vma->vm_start >= start && vma->vm_end <= end)
			vma->vm_flags = vm_flags;
	}
}

int vma_unmap_range(struct mm_struct *mm, struct vm_area_struct *vma,
		    uintptr_t start, uintptr_t end, uintptr_t *unmap_start,
		    uintptr_t *unmap_end)
{
	int ret;
	struct vm_area_struct *right;
	uintptr_t trim_start;
	uintptr_t trim_end;

	if (!vma_overlaps(vma, start, end))
		return 0;

	trim_start = start > vma->vm_start ? start : vma->vm_start;
	trim_end = end < vma->vm_end ? end : vma->vm_end;

	if (unmap_start)
		*unmap_start = trim_start;
	if (unmap_end)
		*unmap_end = trim_end;

	if (trim_start == vma->vm_start && trim_end == vma->vm_end) {
		vma_free_slot(vma);
		return 1;
	}

	if (trim_start == vma->vm_start) {
		if (vma->vm_file)
			vma->vm_offset = vma_offset_at(vma, trim_end);
		vma->vm_start = trim_end;
		return 1;
	}

	if (trim_end == vma->vm_end) {
		vma->vm_end = trim_start;
		return 1;
	}

	ret = vma_split_at(mm, vma, trim_start);
	if (ret < 0)
		return ret;

	right = find_vma(mm, trim_start);
	BUG_ON(!right || right == vma);

	if (right->vm_file)
		right->vm_offset = vma_offset_at(right, trim_end);
	right->vm_start = trim_end;
	return 1;
}

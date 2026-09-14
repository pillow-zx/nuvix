/*
 * mm/vma.c - address-space-owned VMA tree
 */

#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/fdtable.h>
#include <nuvix/string.h>
#include <nuvix/printk.h>
#include <nuvix/page.h>

#include "internal.h"

struct vm_area_struct *vma_first(struct mm_struct *mm)
{
	struct rb_node *n = rb_first(&mm->vmas);
	return n ? rb_entry(n, struct vm_area_struct, node) : NULL;
}

struct vm_area_struct *vma_next(struct vm_area_struct *vma)
{
	struct rb_node *n = vma ? rb_next(&vma->node) : NULL;
	return n ? rb_entry(n, struct vm_area_struct, node) : NULL;
}

void vma_publish(struct vm_area_struct *vma)
{
	struct rb_node **link = &vma->owner->vmas.node, *parent = NULL;
	while (*link) {
		struct vm_area_struct *other = rb_entry(*link, struct vm_area_struct, node);
		parent = *link;
		BUG_ON(vma->vm_start < other->vm_end && other->vm_start < vma->vm_end);
		link = vma->vm_start < other->vm_start ? &parent->left : &parent->right;
	}
	rb_link_node(&vma->node, parent, link);
	rb_insert_color(&vma->node, &vma->owner->vmas);
	vma->used = true;
	vma->owner->map_sequence++;
	mm_anon_register(vma);
	mm_anon_update(vma);
}

int vma_reserve(struct mm_struct *mm, int count)
{
	for (int have = vma_free_slot_count(mm); have < count; have++) {
		struct vm_area_struct *vma = kmalloc(sizeof(*vma), ALLOC_NOWAIT);
		if (!vma)
			return -ENOMEM;
		list_add(&vma->spare, &mm->vma_spares);
	}
	return 0;
}

void vma_discard_spares(struct mm_struct *mm)
{
	while (!list_empty(&mm->vma_spares)) {
		struct vm_area_struct *vma = list_first_entry(&mm->vma_spares, struct vm_area_struct, spare);
		list_del(&vma->spare);
		kfree(vma);
	}
}


__always_inline __pure
static inline bool vma_can_merge(const struct vm_area_struct *a,
				 const struct vm_area_struct *b)
{
	if (!a || !b || a == b)
		return false;
	if (!a->used || !b->used)
		return false;
	if (a->vm_flags != b->vm_flags || a->vm_type != b->vm_type)
		return false;
	if (a->vm_file != b->vm_file || a->vm_anon != b->vm_anon || a->vm_shared != b->vm_shared)
		return false;
	if (a->vm_file || a->vm_anon) {
		if (a->vm_end == b->vm_start)
			return vma_offset_at(a, a->vm_end) == b->vm_offset;
		if (b->vm_end == a->vm_start)
			return vma_offset_at(b, b->vm_end) == a->vm_offset;
		return false;
	}
	return a->vm_end == b->vm_start || b->vm_end == a->vm_start;
}

int mm_range_end_page_aligned(uintptr_t start, size_t length, uintptr_t *end)
{
	uintptr_t aligned_len;

	if (!end)
		return -EINVAL;
	if (length > TASK_SIZE)
		return -EINVAL;

	aligned_len = ALIGN_UP(length, PAGE_SIZE);
	if (aligned_len == 0 || start + aligned_len < start)
		return -EINVAL;
	/* A valid length may still exceed the user address space. */
	if (start + aligned_len > TASK_SIZE)
		return -ENOMEM;

	*end = start + aligned_len;
	return 0;
}

struct vm_area_struct *vma_alloc_slot(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	if (list_empty(&mm->vma_spares) && vma_reserve(mm, 1) < 0)
		return NULL;
	vma = list_first_entry(&mm->vma_spares, struct vm_area_struct, spare);
	list_del(&vma->spare);
	memset(vma, 0, sizeof(*vma));
	vma->owner = mm;
	return vma;
}


void vma_free_slot(struct vm_area_struct *vma)
{
	if (vma->used) {
		rb_erase(&vma->node, &vma->owner->vmas);
		vma->owner->map_sequence++;
	}
	mm_anon_unregister(vma);
	file_put(vma->vm_file);
	mm_anon_put(vma->vm_anon);
	kfree(vma);
}


int vma_free_slot_count(struct mm_struct *mm)
{
	struct list_head *node;
	int count = 0;
	list_for_each(node, &mm->vma_spares)
		count++;
	return count;
}


bool vma_range_overlaps(struct mm_struct *mm, uintptr_t start, uintptr_t end)
{
	for_each_vma(vma, mm) {
		if (vma_overlaps(vma, start, end))
			return true;
	}

	return false;
}

bool vma_range_overlaps_other(struct mm_struct *mm,
			      const struct vm_area_struct *skip,
			      uintptr_t start, uintptr_t end)
{
	for_each_vma(vma, mm) {
		if (vma == skip)
			continue;
		if (vma_overlaps(vma, start, end))
			return true;
	}

	return false;
}

void vma_merge_all(struct mm_struct *mm)
{
	struct vm_area_struct *a = vma_first(mm);
	while (a) {
		struct vm_area_struct *b = vma_next(a);
		if (b && vma_can_merge(a, b)) {
			a->vm_end = b->vm_end;
			mm_anon_update(a);
			mm->map_sequence++;
			vma_free_slot(b);
		} else {
			a = b;
		}
	}
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
	tail->used = false;
	tail->anon_registered = false;
	mm_anon_get(tail->vm_anon);
	if (tail->vm_anon)
		tail->vm_offset = vma_offset_at(vma, addr);
	if (tail->vm_file) {
		file_get(tail->vm_file);
		tail->vm_offset = vma_offset_at(vma, addr);
	}
	tail->vm_start = addr;
	vma->vm_end = addr;
	vma_publish(tail);
	mm_anon_update(vma);
	return 0;
}

int vma_munmap_slots_needed(struct mm_struct *mm, uintptr_t start,
			    uintptr_t end)
{
	int needed = 0;

	for_each_vma(vma, mm) {
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

	for_each_vma(vma, mm) {

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

	for_each_vma(vma, mm) {
		ret = vma_split_at(mm, vma, start);
		if (ret < 0)
			return ret;
	}

	for_each_vma(vma, mm) {
		ret = vma_split_at(mm, vma, end);
		if (ret < 0)
			return ret;
	}

	return 0;
}

void vma_update_flags_range(struct mm_struct *mm, uintptr_t start,
			    uintptr_t end, uint32_t vm_flags)
{
	for_each_vma(vma, mm) {

		if (!vma->used)
			continue;
		if (vma->vm_start >= start && vma->vm_end <= end) {
			vma->vm_flags = vm_flags;
			mm->map_sequence++;
		}
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
		rb_erase(&vma->node, &mm->vmas);
		if (vma->vm_file || vma->vm_anon)
			vma->vm_offset = vma_offset_at(vma, trim_end);
		vma->vm_start = trim_end;
		vma_publish(vma);
		return 1;
	}

	if (trim_end == vma->vm_end) {
		vma->vm_end = trim_start;
		mm->map_sequence++;
		mm_anon_update(vma);
		return 1;
	}

	ret = vma_split_at(mm, vma, trim_start);
	if (ret < 0)
		return ret;

	right = find_vma(mm, trim_start);
	BUG_ON(!right || right == vma);

	rb_erase(&right->node, &mm->vmas);
	if (right->vm_file || right->vm_anon)
		right->vm_offset = vma_offset_at(right, trim_end);
	right->vm_start = trim_end;
	vma_publish(right);
	return 1;
}

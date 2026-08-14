/*
 * mm/vmalloc.c - vmalloc 区虚拟内存分配
 */

#include <nuvix/vmalloc.h>
#include <nuvix/bootinfo.h>
#include <nuvix/bitops.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/list.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/processor.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>

#define VMALLOC_SIZE (128UL << 20)

struct vmalloc_area {
	uintptr_t start;
	uintptr_t end;
	bool free;
	struct list_head node;
};

static uintptr_t vmalloc_start;
static uintptr_t vmalloc_end;
static bool vmalloc_ready;
static LIST_HEAD(vmalloc_areas);

static void vmalloc_unmap_pages(uintptr_t start, uintptr_t end)
{
	pte_t *root = kernel_pt();

	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		pte_t *pte = pgtable_lookup(root, va);
		paddr_t pa;

		if (!pte || !pte_is_present(*pte))
			continue;

		pa = pte_phys_addr(*pte);
		*pte = 0;
		flush_tlb_page(va);
		free_page(__va(pa), 0);
	}
}

static struct vmalloc_area *vmalloc_find_area(uintptr_t start)
{
	struct vmalloc_area *area;

	list_for_each_entry (area, &vmalloc_areas, node) {
		if (area->start == start)
			return area;
	}

	return NULL;
}

static size_t vmalloc_area_size(const struct vmalloc_area *area)
{
	return area->end - area->start;
}

static struct vmalloc_area *vmalloc_find_free_area(size_t size)
{
	struct vmalloc_area *area;

	list_for_each_entry (area, &vmalloc_areas, node) {
		if (area->free && vmalloc_area_size(area) >= size)
			return area;
	}

	return NULL;
}

static int vmalloc_split_area(struct vmalloc_area *area, size_t size,
			      enum alloc_mode mode)
{
	struct vmalloc_area *tail;

	if (vmalloc_area_size(area) == size)
		return 0;

	tail = kmalloc(sizeof(*tail), mode);
	if (!tail)
		return -ENOMEM;

	tail->start = area->start + size;
	tail->end = area->end;
	tail->free = true;
	INIT_LIST_HEAD(&tail->node);

	area->end = tail->start;
	list_add(&tail->node, &area->node);
	return 0;
}

static struct vmalloc_area *vmalloc_prev_area(struct vmalloc_area *area)
{
	if (area->node.prev == &vmalloc_areas)
		return NULL;
	return list_entry(area->node.prev, struct vmalloc_area, node);
}

static struct vmalloc_area *vmalloc_next_area(struct vmalloc_area *area)
{
	if (area->node.next == &vmalloc_areas)
		return NULL;
	return list_entry(area->node.next, struct vmalloc_area, node);
}

static void vmalloc_merge_area(struct vmalloc_area *area)
{
	struct vmalloc_area *prev = vmalloc_prev_area(area);
	struct vmalloc_area *next;

	if (prev && prev->free && prev->end == area->start) {
		prev->end = area->end;
		list_del(&area->node);
		kfree(area);
		area = prev;
	}

	next = vmalloc_next_area(area);
	if (next && next->free && area->end == next->start) {
		area->end = next->end;
		list_del(&next->node);
		kfree(next);
	}
}

void vmalloc_init(void)
{
	struct vmalloc_area *area;

	if (vmalloc_ready)
		return;

	area = kmalloc(sizeof(*area), ALLOC_NOWAIT);
	BUG_ON(!area);

	vmalloc_start =
		ALIGN_UP(KERNEL_VBASE + DRAM_BASE + DRAM_SIZE, PAGE_SIZE);
	vmalloc_end = vmalloc_start + VMALLOC_SIZE;
	INIT_LIST_HEAD(&vmalloc_areas);
	area->start = vmalloc_start;
	area->end = vmalloc_end;
	area->free = true;
	INIT_LIST_HEAD(&area->node);
	list_add_tail(&area->node, &vmalloc_areas);
	vmalloc_ready = true;
}

BOOTINFO_BLOCK(vmalloc, void,
	BROW("Vmalloc Region", "%llu MiB @ 0x%016lx",
	     (unsigned long long)(VMALLOC_SIZE >> 20),
	     (unsigned long)vmalloc_start);
)

void *vmalloc(size_t size, enum alloc_mode mode)
{
	struct vmalloc_area *area;
	uintptr_t start;
	uintptr_t end;
	int ret;

	alloc_check(mode);
	if (size == 0)
		return NULL;
	BUG_ON(!vmalloc_ready);
	if (size > VMALLOC_SIZE)
		return NULL;

	size = ALIGN_UP(size, PAGE_SIZE);
	if (size == 0 || size > VMALLOC_SIZE)
		return NULL;

	area = vmalloc_find_free_area(size);
	if (!area)
		return NULL;
	ret = vmalloc_split_area(area, size, mode);
	if (ret < 0)
		return NULL;

	start = area->start;
	end = area->end;

	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		void *page = get_free_page(0, mode);

		if (!page)
			goto fail;

		memset(page, 0, PAGE_SIZE);
		ret = map_page(kernel_pt(), va, __pa((uintptr_t)page),
			       pgprot_kernel(true, true, false));
		if (ret < 0) {
			free_page(page, 0);
			goto fail;
		}
	}

	area->free = false;
	return (void *)start;

fail:
	vmalloc_unmap_pages(start, end);
	area->free = true;
	vmalloc_merge_area(area);
	return NULL;
}

void vfree(void *ptr)
{
	struct vmalloc_area *area;
	uintptr_t start;

	alloc_free_check();
	start = (uintptr_t)ptr;
	area = vmalloc_find_area(start);
	if (!area)
		panic("vfree: invalid address %p", ptr);
	if (area->free)
		panic("vfree: double free %p", ptr);

	vmalloc_unmap_pages(area->start, area->end);
	area->free = true;
	vmalloc_merge_area(area);
}

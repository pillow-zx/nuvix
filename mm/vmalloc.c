/*
 * mm/vmalloc.c - vmalloc 区虚拟内存分配
 */

#include <nuvix/vmalloc.h>
#include <nuvix/mm.h>
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
static DEFINE_SPINLOCK(vmalloc_lock, LOCK_RANK_ALLOC_VMALLOC,
			   LOCK_IRQ_TASK_ONLY);

/* Clears PTEs and frees pages. Must never be called under vmalloc_lock:
 * free_page() rejects a held spinlock, and the PTE slots of a reserved span
 * are disjoint, so concurrent unmaps cannot collide. */
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
	mm_flush_kernel_all();
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

/* Force-create every L1/L0 page-table entry in the vmalloc region before SMP
 * bring-up so runtime map_page() there never allocates a table page (two CPUs
 * mapping concurrently into an unpopulated span would race in pt_walk_create).
 * Runs pre-SMP; leaf PTEs stay zero. */
static void vmalloc_prepopulate_tables(void)
{
	pte_t *root = kernel_pt();

	for (uintptr_t va = vmalloc_start; va < vmalloc_end;
	     va += (2UL << 20)) {
		pte_t *l2e = &root[(va >> 30) & 0x1FF];

		if (!(*l2e & PTE_V)) {
			void *table = get_free_page(0, ALLOC_NOWAIT);

			BUG_ON(!table);
			memset(table, 0, PAGE_SIZE);
			*l2e = PA_TO_PTE(__pa((uintptr_t)table)) | PTE_TABLE;
		}

		pte_t *l1 = (pte_t *)__va(PTE_TO_PA(*l2e));
		pte_t *l1e = &l1[(va >> 21) & 0x1FF];

		if (!(*l1e & PTE_V)) {
			void *table = get_free_page(0, ALLOC_NOWAIT);

			BUG_ON(!table);
			memset(table, 0, PAGE_SIZE);
			*l1e = PA_TO_PTE(__pa((uintptr_t)table)) | PTE_TABLE;
		}
	}

	flush_tlb_all();
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

/* Under vmalloc_lock: merge area with free neighbors, collecting the
 * merged-out area structs (at most two) in detached[] for the caller to kfree
 * after the lock is released. Unused slots stay NULL (kfree(NULL) is a
 * no-op) so the array is always fully initialized. */
static void vmalloc_merge_locked(struct vmalloc_area *area,
				 struct vmalloc_area **detached, size_t *nr)
{
	struct vmalloc_area *prev = vmalloc_prev_area(area);
	struct vmalloc_area *next;

	detached[0] = NULL;
	detached[1] = NULL;
	*nr = 0;
	if (prev && prev->free && prev->end == area->start) {
		prev->end = area->end;
		list_del(&area->node);
		detached[(*nr)++] = area;
		area = prev;
	}

	next = vmalloc_next_area(area);
	if (next && next->free && area->end == next->start) {
		area->end = next->end;
		list_del(&next->node);
		detached[(*nr)++] = next;
	}
}

static void vmalloc_kfree_detached(struct vmalloc_area **detached, size_t nr)
{
	for (size_t i = 0; i < nr; i++)
		kfree(detached[i]);
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

	/* Pre-SMP: force-create the region's page tables so runtime map_page()
	 * never allocates a table page under concurrent mapping. */
	vmalloc_prepopulate_tables();
}

BOOTINFO_BLOCK(vmalloc, void,
	BROW("Vmalloc Region", "%llu MiB @ 0x%016lx",
	     (unsigned long long)(VMALLOC_SIZE >> 20),
	     (unsigned long)vmalloc_start);
)

void *vmalloc(size_t size, enum alloc_mode mode)
{
	struct vmalloc_area *area;
	struct vmalloc_area *tail = NULL;
	struct vmalloc_area *detached[2];
	uintptr_t start;
	uintptr_t end;
	uintptr_t map_end;
	size_t nr_detached;
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

	spin_lock(&vmalloc_lock);
	area = vmalloc_find_free_area(size);
	if (!area) {
		spin_unlock(&vmalloc_lock);
		return NULL;
	}
	/* Reserve the whole span before splitting so no other CPU can grab or
	 * merge it while the split tail is allocated below. */
	area->free = false;
	start = area->start;
	end = area->end;
	spin_unlock(&vmalloc_lock);

	if (end - start != size) {
		/* The split tail is allocated outside the lock (kmalloc may
		 * allocate). On failure, release the reservation. */
		tail = kmalloc(sizeof(*tail), mode);
		if (!tail) {
			spin_lock(&vmalloc_lock);
			area->free = true;
			vmalloc_merge_locked(area, detached, &nr_detached);
			spin_unlock(&vmalloc_lock);
			vmalloc_kfree_detached(detached, nr_detached);
			return NULL;
		}

		spin_lock(&vmalloc_lock);
		tail->start = start + size;
		tail->end = end;
		tail->free = true;
		INIT_LIST_HEAD(&tail->node);
		area->end = tail->start;
		list_add(&tail->node, &area->node);
		spin_unlock(&vmalloc_lock);
	}
	map_end = area->end;

	for (uintptr_t va = start; va < map_end; va += PAGE_SIZE) {
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

	return (void *)start;

fail:
	vmalloc_unmap_pages(start, map_end);
	spin_lock(&vmalloc_lock);
	area->free = true;
	vmalloc_merge_locked(area, detached, &nr_detached);
	spin_unlock(&vmalloc_lock);
	vmalloc_kfree_detached(detached, nr_detached);
	return NULL;
}

void vfree(void *ptr)
{
	struct vmalloc_area *area;
	struct vmalloc_area *detached[2];
	uintptr_t start;
	uintptr_t end;
	size_t nr_detached;

	alloc_free_check();
	start = (uintptr_t)ptr;

	spin_lock(&vmalloc_lock);
	area = vmalloc_find_area(start);
	if (!area)
		panic("vfree: invalid address %p", ptr);
	if (area->free)
		panic("vfree: double free %p", ptr);
	/* Keep the span reserved while unmapping below so a concurrent vmalloc
	 * cannot reuse these VAs or their physical pages. */
	end = area->end;
	spin_unlock(&vmalloc_lock);

	vmalloc_unmap_pages(start, end);

	spin_lock(&vmalloc_lock);
	area->free = true;
	vmalloc_merge_locked(area, detached, &nr_detached);
	spin_unlock(&vmalloc_lock);
	vmalloc_kfree_detached(detached, nr_detached);
}

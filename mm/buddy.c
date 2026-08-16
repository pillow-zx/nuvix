/*
 * mm/buddy.c - 伙伴系统（物理页分配）
 */

#include <nuvix/buddy.h>
#include <nuvix/bootinfo.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>
#include <nuvix/bitops.h>
#include <nuvix/tools.h>
#include <nuvix/page.h>

struct page *mem_map;
struct free_area free_area[MAX_ORDER + 1];
static DEFINE_SPINLOCK(buddy_lock, LOCK_RANK_ALLOC_BUDDY,
			   LOCK_IRQ_TASK_ONLY);
static size_t total_pages;
static size_t nr_free_pages;

static inline size_t page_to_pfn(const struct page *page)
{
	return (size_t)(page - mem_map);
}

static inline struct page *pfn_to_page(size_t pfn)
{
	return &mem_map[pfn];
}

static inline void *pfn_to_virt(size_t pfn)
{
	return __va(DRAM_BASE + pfn * PAGE_SIZE);
}

static inline size_t virt_to_pfn(void *addr)
{
	return (__pa((uintptr_t)addr) - DRAM_BASE) / PAGE_SIZE;
}

static bool virt_to_pfn_checked(const void *addr, size_t *pfn)
{
	uintptr_t va = (uintptr_t)addr;
	uintptr_t direct_start = (uintptr_t)__va(DRAM_BASE);
	uintptr_t direct_end = direct_start + DRAM_SIZE;

	if (!addr || !IS_ALIGNED(va, PAGE_SIZE))
		return false;
	if (va < direct_start || va >= direct_end)
		return false;

	*pfn = (va - direct_start) / PAGE_SIZE;
	return *pfn < total_pages;
}

static void buddy_add_free_block(size_t pfn, uint32_t order, bool tail)
{
	struct page *page = pfn_to_page(pfn);

	if (pfn >= total_pages)
		panic("buddy: add free pfn %zu out of range", pfn);
	if (order > MAX_ORDER)
		panic("buddy: add free order %u out of range", order);

	page->flags = BIT(PG_BUDDY);
	page->order = order;
	refcount_set(&page->refcount, 0);
	if (tail)
		list_add_tail(&page->lru, &free_area[order].free_list);
	else
		list_add(&page->lru, &free_area[order].free_list);
	free_area[order].nr_free++;
}

static void buddy_remove_free_block(struct page *page)
{
	BUG_ON(!page_test_flag(page, PG_BUDDY));
	list_del(&page->lru);
	page_clear_flag(page, PG_BUDDY);
	free_area[page->order].nr_free--;
}

static void buddy_free_page(size_t pfn, uint32_t order)
{
	size_t freed_pages = 1UL << order;

	while (order < MAX_ORDER) {
		size_t buddy_pfn = pfn ^ (1UL << order);
		struct page *buddy;

		if (buddy_pfn >= total_pages)
			break;

		buddy = pfn_to_page(buddy_pfn);
		if (!page_test_flag(buddy, PG_BUDDY))
			break;
		if (buddy->order != order)
			break;

		buddy_remove_free_block(buddy);
		pfn = pfn < buddy_pfn ? pfn : buddy_pfn;
		order++;
	}

	buddy_add_free_block(pfn, order, false);
	nr_free_pages += freed_pages;
}

static void page_ref_check(const struct page *page)
{
	BUG_ON(page < mem_map || page >= mem_map + total_pages);
	BUG_ON(page_test_flag(page, PG_BUDDY));
	BUG_ON(page_test_flag(page, PG_RESERVED));
	BUG_ON(page_test_flag(page, PG_SLAB));
	BUG_ON(page->order != 0);
}

void buddy_init(void)
{
	void *mem_start = bootmem_end();

	/* Pre-SMP boot phase: no secondary hart is online, so the free lists
	 * are not yet shared. buddy_lock is not required here. */
	total_pages = DRAM_SIZE / PAGE_SIZE;


	mem_map = (struct page *)mem_start;


	for (size_t i = 0; i < total_pages; i++) {
		mem_map[i].flags = BIT(PG_RESERVED);
		mem_map[i].order = 0;
		refcount_set(&mem_map[i].refcount, 0);
		INIT_LIST_HEAD(&mem_map[i].lru);
	}


	for (uint32_t i = 0; i <= MAX_ORDER; i++) {
		INIT_LIST_HEAD(&free_area[i].free_list);
		free_area[i].nr_free = 0;
	}


	uintptr_t mem_map_bytes = total_pages * sizeof(struct page);
	vaddr_t free_start_va =
		ALIGN_UP((uintptr_t)mem_start + mem_map_bytes, PAGE_SIZE);
	paddr_t free_start_pa = __pa(free_start_va);
	size_t free_idx = (free_start_pa - DRAM_BASE) / PAGE_SIZE;
	size_t remaining = total_pages - free_idx;


	nr_free_pages = 0;
	size_t idx = free_idx;

	while (remaining > 0) {

		uint32_t order = MAX_ORDER;
		while (order > 0 && ((idx & ((1UL << order) - 1)) != 0 ||
				     remaining < (1UL << order)))
			order--;

		buddy_add_free_block(idx, order, true);

		nr_free_pages += (1UL << order);
		idx += (1UL << order);
		remaining -= (1UL << order);
	}
}

BOOTINFO_BLOCK(buddy, void,
	BROW("Buddy Free", "%llu MiB (%lu pages)",
	     (unsigned long long)(nr_free_pages * PAGE_SIZE >> 20),
	     (unsigned long)nr_free_pages);
)

void *get_free_page(uint32_t order, enum alloc_mode mode)
{
	struct list_head *node;
	struct page *page;

	alloc_check(mode);
	spin_lock(&buddy_lock);
	if (order > MAX_ORDER)
		goto out;

	uint32_t cur = order;
	while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list))
		cur++;

	if (cur > MAX_ORDER)
		goto out;

	node = free_area[cur].free_list.next;
	page = list_entry(node, struct page, lru);
	if (page < mem_map || page >= mem_map + total_pages)
		panic("buddy: corrupt free list order %u node=%p page=%p",
		      cur, node, page);
	if (!page_test_flag(page, PG_BUDDY))
		panic("buddy: non-free page on free list order %u pfn=%lu",
		      cur, (unsigned long)page_to_pfn(page));
	if (page->order != cur)
		panic("buddy: free list order %u contains pfn=%lu order=%u",
		      cur, (unsigned long)page_to_pfn(page), page->order);
	buddy_remove_free_block(page);

	while (cur > order) {
		cur--;
		size_t buddy_pfn = page_to_pfn(page) + (1UL << cur);
		buddy_add_free_block(buddy_pfn, cur, false);
	}

	page->flags = 0;
	page->order = order;
	refcount_set(&page->refcount, 1);

	nr_free_pages -= (1UL << order);

	spin_unlock(&buddy_lock);
	return pfn_to_virt(page_to_pfn(page));

out:
	spin_unlock(&buddy_lock);
	return NULL;
}

void free_page(void *addr, uint32_t order)
{
	size_t pfn;
	struct page *page;

	alloc_free_check();
	spin_lock(&buddy_lock);
	if (order > MAX_ORDER)
		panic("free_page: order %d > MAX_ORDER", order);

	if (!virt_to_pfn_checked(addr, &pfn))
		panic("free_page: invalid page address %p", addr);
	if (pfn + (1UL << order) > total_pages)
		panic("free_page: pfn %zu order %u out of range", pfn, order);
	if (pfn & ((1UL << order) - 1))
		panic("free_page: pfn %zu is not order %u aligned", pfn, order);

	page = pfn_to_page(pfn);
	if (page_test_flag(page, PG_BUDDY))
		panic("free_page: double free pfn %zu", pfn);
	if (page_test_flag(page, PG_RESERVED))
		panic("free_page: reserved pfn %zu", pfn);
	if (page_test_flag(page, PG_SLAB))
		panic("free_page: slab pfn %zu", pfn);
	if (refcount_read(&page->refcount) != 1)
		panic("free_page: pfn %zu still referenced", pfn);
	if (page->order != order)
		panic("free_page: pfn %zu order %u != %u", pfn, page->order,
		      order);

	BUG_ON(!refcount_dec_and_test(&page->refcount));
	page->flags = 0;
	buddy_free_page(pfn, order);
	spin_unlock(&buddy_lock);
}

void page_get(struct page *page)
{
	page_ref_check(page);
	refcount_inc(&page->refcount);
}

void page_put(struct page *page)
{
	page_ref_check(page);
	if (!refcount_dec_and_test(&page->refcount))
		return;

	alloc_free_check();
	spin_lock(&buddy_lock);
	page->flags = 0;
	buddy_free_page(page_to_pfn(page), 0);
	spin_unlock(&buddy_lock);
}

size_t buddy_free_pages(void)
{
	size_t nr;

	spin_lock(&buddy_lock);
	nr = nr_free_pages;
	spin_unlock(&buddy_lock);
	return nr;
}

struct page *virt_to_page(const void *addr)
{
	size_t pfn;

	if (!virt_to_pfn_checked(addr, &pfn))
		return NULL;
	return pfn_to_page(pfn);
}

void *page_to_virt(const struct page *page)
{
	BUG_ON(page < mem_map || page >= mem_map + total_pages);
	return pfn_to_virt(page_to_pfn(page));
}

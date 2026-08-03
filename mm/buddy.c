/*
 * mm/buddy.c - 伙伴系统（物理页分配）
 */

#include <kernel/buddy.h>
#include <kernel/page.h>
#include <kernel/printk.h>
#include <kernel/bitops.h>
#include <kernel/tools.h>
#include <kernel/page.h>

struct page *mem_map;
struct free_area free_area[MAX_ORDER + 1];
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
	page->refcount = 0;
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

void buddy_init(void)
{
	void *mem_start = bootmem_end();

	total_pages = DRAM_SIZE / PAGE_SIZE;


	mem_map = (struct page *)mem_start;


	for (size_t i = 0; i < total_pages; i++) {
		mem_map[i].flags = BIT(PG_RESERVED);
		mem_map[i].order = 0;
		mem_map[i].refcount = 0;
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

	pr_info("buddy: %d pages free (%d MB)\n", (int)nr_free_pages,
		(int)(nr_free_pages * PAGE_SIZE >> 20));
}

void *get_free_page(uint32_t order, enum alloc_mode mode)
{
	struct list_head *node;
	struct page *page;

	alloc_check(mode);
	if (order > MAX_ORDER)
		return NULL;


	uint32_t cur = order;
	while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list))
		cur++;

	if (cur > MAX_ORDER)
		return NULL;


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
	page->refcount = 1;

	nr_free_pages -= (1UL << order);

	return pfn_to_virt(page_to_pfn(page));
}

void free_page(void *addr, uint32_t order)
{
	size_t freed_pages;
	size_t pfn;
	struct page *page;

	alloc_free_check();
	if (order > MAX_ORDER)
		panic("free_page: order %d > MAX_ORDER", order);

	freed_pages = 1UL << order;

	if (!virt_to_pfn_checked(addr, &pfn))
		panic("free_page: invalid page address %p", addr);
	if (pfn + freed_pages > total_pages)
		panic("free_page: pfn %zu order %u out of range", pfn, order);
	if (pfn & (freed_pages - 1))
		panic("free_page: pfn %zu is not order %u aligned", pfn, order);

	page = pfn_to_page(pfn);
	if (page_test_flag(page, PG_BUDDY))
		panic("free_page: double free pfn %zu", pfn);
	if (page_test_flag(page, PG_RESERVED))
		panic("free_page: reserved pfn %zu", pfn);
	if (page_test_flag(page, PG_SLAB))
		panic("free_page: slab pfn %zu", pfn);
	if (page->refcount == 0)
		panic("free_page: unallocated pfn %zu", pfn);
	if (page->order != order)
		panic("free_page: pfn %zu order %u != %u", pfn, page->order,
		      order);


	page->flags = 0;
	page->refcount = 0;


	while (order < MAX_ORDER) {
		size_t buddy_pfn = pfn ^ (1UL << order);


		if (buddy_pfn >= total_pages)
			break;

		struct page *buddy = pfn_to_page(buddy_pfn);


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

size_t buddy_free_pages(void)
{
	return nr_free_pages;
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

#ifdef KERNEL_SELFTEST
void buddy_test_validate(void)
{
	for (uint32_t order = 0; order <= MAX_ORDER; order++) {
		struct list_head *head = &free_area[order].free_list;
		struct list_head *pos;
		uint32_t count = 0;

		if (!head->next || !head->prev)
			panic("buddy: free list order %u has null links", order);

		list_for_each (pos, head) {
			struct page *page;

			if (++count > total_pages)
				panic("buddy: free list order %u cycle", order);
			if (!pos || !pos->next || !pos->prev)
				panic("buddy: free list order %u null node link",
				      order);
			if (pos->next->prev != pos || pos->prev->next != pos)
				panic("buddy: free list order %u broken links",
				      order);

			page = list_entry(pos, struct page, lru);
			if (page < mem_map || page >= mem_map + total_pages)
				panic("buddy: free list order %u bad page %p",
				      order, page);
			if (!page_test_flag(page, PG_BUDDY))
				panic("buddy: free list order %u pfn=%lu "
				      "flags=0x%x ref=%u is not free",
				      order, (unsigned long)page_to_pfn(page),
				      page->flags, page->refcount);
			if (page->order != order)
				panic("buddy: free list order %u pfn=%lu has "
				      "order=%u",
				      order, (unsigned long)page_to_pfn(page),
				      page->order);
			if (page->refcount != 0)
				panic("buddy: free list order %u pfn=%lu has "
				      "ref=%u",
				      order, (unsigned long)page_to_pfn(page),
				      page->refcount);
		}

		if (count != free_area[order].nr_free)
			panic("buddy: free list order %u count=%u nr_free=%u",
			      order, count, free_area[order].nr_free);
	}
}
#endif

/*
 * mm/buddy.c - 伙伴系统（物理页分配）
 */

#include <nuvix/buddy.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>
#include <nuvix/bitops.h>
#include <nuvix/tools.h>
#include <nuvix/bootmem.h>

struct page *mem_map;
struct free_area free_area[MAX_ORDER + 1];
static DEFINE_SPINLOCK(buddy_lock, LOCK_RANK_ALLOC_BUDDY, LOCK_IRQ_TASK_ONLY);
static size_t total;
static size_t nr_free_pages;

static void buddy_add_block(size_t pfn, uint32_t order, bool tail)
{
	struct page *page;

	if (unlikely(pfn >= total))
		panic("buddy: add free pfn %zu out of range", pfn);
	if (unlikely(order > MAX_ORDER))
		panic("buddy: add free order %u out of range", order);
	page = &mem_map[pfn];

	page->flags = BIT(PG_BUDDY);
	page->order = order;
	refcount_set(&page->refcount, 0);
	if (tail)
		list_add_tail(&page->lru, &free_area[order].free_list);
	else
		list_add(&page->lru, &free_area[order].free_list);
	free_area[order].nr_free++;
}

static void buddy_remove_block(struct page *page)
{
	BUG_ON(!test_bit(page->flags, PG_BUDDY));
	list_del(&page->lru);
	clr_bit(page->flags, PG_BUDDY);
	free_area[page->order].nr_free--;
}

static void buddy_free_page(size_t pfn, uint32_t order)
{
	size_t freed_pages = 1UL << order;

	while (order < MAX_ORDER) {
		size_t buddy_pfn = pfn ^ (1UL << order);
		struct page *buddy;

		if (buddy_pfn >= total)
			break;

		buddy = &mem_map[buddy_pfn];
		if (!test_bit(buddy->flags, PG_BUDDY))
			break;
		if (buddy->order != order)
			break;

		buddy_remove_block(buddy);
		pfn = pfn < buddy_pfn ? pfn : buddy_pfn;
		order++;
	}

	buddy_add_block(pfn, order, false);
	nr_free_pages += freed_pages;
}

void buddy_init(void)
{
	/* Pre-SMP boot phase: no secondary hart is online, so the free lists
	 * are not yet shared. buddy_lock is not required here. */
	total = ram_size / PAGE_SIZE;

	if (total > SIZE_MAX / sizeof(struct page))
		panic("buddy: page metadata size overflow");
	mem_map = bootmem_alloc(total * sizeof(struct page));

	for (size_t i = 0; i < total; i++) {
		mem_map[i].flags = BIT(PG_RESERVED);
		mem_map[i].order = 0;
		refcount_set(&mem_map[i].refcount, 0);
		INIT_LIST_HEAD(&mem_map[i].lru);
	}


	for (uint32_t i = 0; i <= MAX_ORDER; i++) {
		INIT_LIST_HEAD(&free_area[i].free_list);
		free_area[i].nr_free = 0;
	}


	nr_free_pages = 0;
	for (size_t idx = 0; idx < total;) {
		if (bootmem_reserved(ram_base + idx * PAGE_SIZE, PAGE_SIZE)) {
			idx++;
			continue;
		}
		size_t end = idx + 1;
		while (end < total && !bootmem_reserved(ram_base + end * PAGE_SIZE, PAGE_SIZE))
			end++;
		while (idx < end) {
			uint32_t order = MAX_ORDER;
			while (order && ((idx & ((1UL << order) - 1)) ||
					end - idx < (1UL << order)))
				order--;
			buddy_add_block(idx, order, true);
			nr_free_pages += 1UL << order;
			idx += 1UL << order;
		}
	}
	bootmem_finish();
}

__hot
void *get_page(uint32_t order, enum alloc_mode mode)
{
	struct list_head *node;
	struct page *page;

	alloc_check(mode);
	spin_lock(&buddy_lock);
	if (unlikely(order > MAX_ORDER))
		goto out;

	uint32_t cur = order;
	while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list))
		cur++;

	if (unlikely(cur > MAX_ORDER))
		goto out;

	node = free_area[cur].free_list.next;
	page = list_entry(node, struct page, lru);
	if (unlikely(page < mem_map || page >= mem_map + total))
		panic("buddy: corrupt free list order %u node=%p page=%p",
		      cur, node, page);
	if (unlikely(!test_bit(page->flags, PG_BUDDY)))
		panic("buddy: non-free page on free list order %u pfn=%lu",
		      cur, (unsigned long)(page - mem_map));
	if (unlikely(page->order != cur))
		panic("buddy: free list order %u contains pfn=%lu order=%u",
		      cur, (unsigned long)(page - mem_map), page->order);
	buddy_remove_block(page);

	while (cur > order) {
		cur--;
		size_t buddy_pfn = (size_t)(page - mem_map) + (1UL << cur);
		buddy_add_block(buddy_pfn, cur, false);
	}

	size_t pfn = (size_t)(page - mem_map);

	page->flags = 0;
	page->order = order;
	refcount_set(&page->refcount, 1);
	atomic_set(&page->io_write_pins, 0);

	nr_free_pages -= (1UL << order);

	spin_unlock(&buddy_lock);
	return __va(ram_base + pfn * PAGE_SIZE);

out:
	spin_unlock(&buddy_lock);
	return NULL;
}

__hot
void free_page(void *addr, uint32_t order)
{
	size_t pfn;
	struct page *page;

	alloc_free_check();
	spin_lock(&buddy_lock);
	if (unlikely(order > MAX_ORDER))
		panic("free_page: order %d > MAX_ORDER", order);

	pfn = (__pa((uintptr_t)addr) - ram_base) / PAGE_SIZE;
	if (unlikely(pfn + (1UL << order) > total))
		panic("free_page: pfn %zu order %u out of range", pfn, order);
	if (unlikely(pfn & ((1UL << order) - 1)))
		panic("free_page: pfn %zu is not order %u aligned", pfn, order);

	page = &mem_map[pfn];
	if (unlikely(test_bit(page->flags, PG_BUDDY)))
		panic("free_page: double free pfn %zu", pfn);
	if (unlikely(test_bit(page->flags, PG_RESERVED)))
		panic("free_page: reserved pfn %zu", pfn);
	if (unlikely(test_bit(page->flags, PG_SLAB)))
		panic("free_page: slab pfn %zu", pfn);
	if (unlikely(refcount_read(&page->refcount) != 1))
		panic("free_page: pfn %zu still referenced", pfn);
	if (unlikely(page->order != order))
		panic("free_page: pfn %zu order %u != %u", pfn, page->order,
		      order);

	BUG_ON(!refcount_dec_and_test(&page->refcount));
	page->flags = 0;
	buddy_free_page(pfn, order);
	spin_unlock(&buddy_lock);
}

__hot
void page_get(struct page *page)
{
	refcount_inc(&page->refcount);
}

__hot
void page_put(struct page *page)
{
	if (!refcount_dec_and_test(&page->refcount))
		return;

	alloc_free_check();
	spin_lock(&buddy_lock);
	page->flags = 0;
	buddy_free_page((size_t)(page - mem_map), 0);
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
	size_t pfn = (__pa((uintptr_t)addr) - ram_base) / PAGE_SIZE;

	if (pfn >= total)
		return NULL;
	return &mem_map[pfn];
}

void *page_to_virt(const struct page *page)
{
	size_t pfn;

	BUG_ON(page < mem_map || page >= mem_map + total);
	pfn = (size_t)(page - mem_map);
	return __va(ram_base + pfn * PAGE_SIZE);
}

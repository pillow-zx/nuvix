#include "mm_fixture.h"

int test_vmalloc_alloc_writable_pages(void)
{
	void *ptr = NULL;
	uintptr_t va;
	uintptr_t vmalloc_start;
	uintptr_t vmalloc_end;
	pte_t *pte0;
	pte_t *pte1;

	TEST_BEGIN("vmalloc: alloc writable pages");
	{
		vmalloc_start = ALIGN_UP(KERNEL_VBASE + DRAM_BASE + DRAM_SIZE,
					 PAGE_SIZE);
		vmalloc_end = vmalloc_start + MM_TEST_VMALLOC_SIZE;

		ptr = vmalloc(PAGE_SIZE + 1, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(ptr);
		TEST_ASSERT_ALIGNED(ptr, PAGE_SIZE);

		va = (uintptr_t)ptr;
		TEST_ASSERT(va >= vmalloc_start);
		TEST_ASSERT(va + 2 * PAGE_SIZE <= vmalloc_end);

		((uint8_t *)ptr)[0] = 0x5a;
		((uint8_t *)ptr)[PAGE_SIZE] = 0xa5;
		TEST_ASSERT(((uint8_t *)ptr)[0] == 0x5a);
		TEST_ASSERT(((uint8_t *)ptr)[PAGE_SIZE] == 0xa5);

		pte0 = pagetable_lookup_current(va);
		pte1 = pagetable_lookup_current(va + PAGE_SIZE);
		TEST_ASSERT_NOT_NULL(pte0);
		TEST_ASSERT_NOT_NULL(pte1);
		TEST_ASSERT(pte_is_present(*pte0));
		TEST_ASSERT(pte_is_present(*pte1));
		TEST_ASSERT(!pte_is_user_page(*pte0));
		TEST_ASSERT(!pte_is_user_page(*pte1));
		TEST_ASSERT(!pte_allows_user_exec(*pte0));
		TEST_ASSERT(!pte_allows_user_exec(*pte1));

		vfree(ptr);
		ptr = NULL;
	}
	TEST_END("vmalloc: alloc writable pages");
	return __test_ret;
fail:
	if (ptr)
		vfree(ptr);
	TEST_FAIL("vmalloc: alloc writable pages", "see above");

	return __test_ret;
}

int test_vmalloc_vfree_reuses_range(void)
{
	void *warm = NULL;
	void *first = NULL;
	void *second = NULL;
	uintptr_t first_addr = 0;
	size_t free_before;

	TEST_BEGIN("vmalloc: vfree reuses range");
	{
		warm = vmalloc(PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(warm);
		vfree(warm);
		warm = NULL;

		free_before = buddy_free_pages();
		first = vmalloc(2 * PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(first);
		TEST_ASSERT_EQ(buddy_free_pages(), free_before - 2);
		first_addr = (uintptr_t)first;

		vfree(first);
		first = NULL;
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);

		second = vmalloc(2 * PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(second);
		TEST_ASSERT_EQ((uintptr_t)second, first_addr);

		vfree(second);
		second = NULL;
		first = NULL;
	}
	TEST_END("vmalloc: vfree reuses range");
	return __test_ret;
fail:
	if (warm)
		vfree(warm);
	if (first && first != second)
		vfree(first);
	if (second)
		vfree(second);
	TEST_FAIL("vmalloc: vfree reuses range", "see above");

	return __test_ret;
}

int test_vmalloc_free_merges_adjacent_ranges(void)
{
	void *a = NULL;
	void *b = NULL;
	void *c = NULL;
	void *merged = NULL;
	uintptr_t a_addr;

	TEST_BEGIN("vmalloc: free merges adjacent ranges");
	{
		a = vmalloc(PAGE_SIZE, ALLOC_NOWAIT);
		b = vmalloc(PAGE_SIZE, ALLOC_NOWAIT);
		c = vmalloc(PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(a);
		TEST_ASSERT_NOT_NULL(b);
		TEST_ASSERT_NOT_NULL(c);
		a_addr = (uintptr_t)a;

		vfree(a);
		a = NULL;
		vfree(b);
		b = NULL;

		merged = vmalloc(2 * PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ((uintptr_t)merged, a_addr);

		vfree(merged);
		merged = NULL;
		vfree(c);
		c = NULL;
	}
	TEST_END("vmalloc: free merges adjacent ranges");
	return __test_ret;
fail:
	if (a)
		vfree(a);
	if (b)
		vfree(b);
	if (c)
		vfree(c);
	if (merged)
		vfree(merged);
	TEST_FAIL("vmalloc: free merges adjacent ranges", "see above");

	return __test_ret;
}

int test_vmalloc_mapping_failure_rolls_back(void)
{
	void *marker = NULL;
	void *padding = NULL;
	void *prefix = NULL;
	void *probe = NULL;
	size_t prefix_size = MM_TEST_VMALLOC_L0_SIZE - PAGE_SIZE;
	size_t padding_size;
	uintptr_t cursor;
	size_t free_before;
	uintptr_t failed_start;
	pte_t *pte;

	TEST_BEGIN("vmalloc: mapping failure rolls back");
	{
		marker = vmalloc(PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(marker);
		cursor = (uintptr_t)marker;
		vfree(marker);
		marker = NULL;

		padding_size =
			ALIGN_UP(cursor, MM_TEST_VMALLOC_L0_SIZE) - cursor;
		if (padding_size) {
			padding = vmalloc(padding_size, ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(padding);
			TEST_ASSERT_EQ((uintptr_t)padding, cursor);
		}

		prefix = vmalloc(prefix_size, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(prefix);
		TEST_ASSERT_ALIGNED(prefix, MM_TEST_VMALLOC_L0_SIZE);

		failed_start = (uintptr_t)prefix + prefix_size;
		free_before = buddy_free_pages();

		pagetable_test_fail_alloc_after(0);
		TEST_ASSERT_NULL(vmalloc(2 * PAGE_SIZE, ALLOC_NOWAIT));
		pagetable_test_clear_alloc_failure();

		TEST_ASSERT_EQ(buddy_free_pages(), free_before);
		pte = pagetable_lookup_current(failed_start);
		TEST_ASSERT_NOT_NULL(pte);
		TEST_ASSERT(!pte_is_present(*pte));
		TEST_ASSERT_NULL(
			pagetable_lookup_current(failed_start + PAGE_SIZE));

		probe = vmalloc(3 * PAGE_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(probe);
		TEST_ASSERT_EQ((uintptr_t)probe, failed_start);

		vfree(probe);
		probe = NULL;
		vfree(prefix);
		prefix = NULL;
		if (padding) {
			vfree(padding);
			padding = NULL;
		}
	}
	TEST_END("vmalloc: mapping failure rolls back");
	return __test_ret;
fail:
	pagetable_test_clear_alloc_failure();
	if (probe)
		vfree(probe);
	if (prefix)
		vfree(prefix);
	if (padding)
		vfree(padding);
	if (marker)
		vfree(marker);
	TEST_FAIL("vmalloc: mapping failure rolls back", "see above");

	return __test_ret;
}

#include "mm_fixture.h"

int test_map_page_first_table_oom_rolls_back(void)
{
	TEST_BEGIN("map_page: first table OOM rolls back");
	{
		pte_t *root = get_free_page(0, ALLOC_NOWAIT);
		void *page = get_free_page(0, ALLOC_NOWAIT);
		size_t free_before;
		int ret;

		TEST_ASSERT_NOT_NULL(root);
		TEST_ASSERT_NOT_NULL(page);
		memset(root, 0, PAGE_SIZE);

		free_before = buddy_free_pages();
		pagetable_test_fail_alloc_after(0);
		ret = map_page(root, MM_TEST_BASE, __pa((uintptr_t)page),
			       pgprot_user(true, true, false));
		pagetable_test_clear_alloc_failure();

		TEST_ASSERT_EQ(ret, -ENOMEM);
		TEST_ASSERT_NULL(pagetable_walk(root, MM_TEST_BASE, false));
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);

		free_page(page, 0);
		free_page(root, 0);
	}
	TEST_END("map_page: first table OOM rolls back");
	return __test_ret;
fail:
	pagetable_test_clear_alloc_failure();
	TEST_FAIL("map_page: first table OOM rolls back", "see above");

	return __test_ret;
}

int test_map_page_second_table_oom_rolls_back(void)
{
	TEST_BEGIN("map_page: second table OOM rolls back");
	{
		pte_t *root = get_free_page(0, ALLOC_NOWAIT);
		void *page = get_free_page(0, ALLOC_NOWAIT);
		size_t free_before;
		int ret;

		TEST_ASSERT_NOT_NULL(root);
		TEST_ASSERT_NOT_NULL(page);
		memset(root, 0, PAGE_SIZE);

		free_before = buddy_free_pages();
		pagetable_test_fail_alloc_after(1);
		ret = map_page(root, MM_TEST_BASE, __pa((uintptr_t)page),
			       pgprot_user(true, true, false));
		pagetable_test_clear_alloc_failure();

		TEST_ASSERT_EQ(ret, -ENOMEM);
		TEST_ASSERT_NULL(pagetable_walk(root, MM_TEST_BASE, false));
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);

		free_page(page, 0);
		free_page(root, 0);
	}
	TEST_END("map_page: second table OOM rolls back");
	return __test_ret;
fail:
	pagetable_test_clear_alloc_failure();
	TEST_FAIL("map_page: second table OOM rolls back", "see above");

	return __test_ret;
}

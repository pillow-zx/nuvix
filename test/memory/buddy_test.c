#include <kernel/test.h>
#include <kernel/buddy.h>
#include <kernel/task.h>

#include "../ktest.h"

int test_buddy_single_page(void)
{
	TEST_BEGIN("buddy: single page alloc/free");
	{
		void *p = get_free_page(0, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(p);
		TEST_ASSERT_ALIGNED(p, PAGE_SIZE);


		memset(p, 0xBB, PAGE_SIZE);
		TEST_ASSERT(((uint8_t *)p)[0] == 0xBB);
		TEST_ASSERT(((uint8_t *)p)[PAGE_SIZE - 1] == 0xBB);

		free_page(p, 0);
	}
	TEST_END("buddy: single page alloc/free");
	return __test_ret;
fail:
	TEST_FAIL("buddy: single page alloc/free", "see above");

	return __test_ret;
}

int test_buddy_multi_order(void)
{
	TEST_BEGIN("buddy: multi-order alloc/free");
	{
		void *ptrs[5];

		for (uint32_t order = 0; order <= 4; order++) {
			size_t size = (size_t)PAGE_SIZE << order;
			size_t align = size;

			ptrs[order] = get_free_page(order, ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(ptrs[order]);
			TEST_ASSERT_ALIGNED(ptrs[order], align);


			memset(ptrs[order], 0xCC, size);
			TEST_ASSERT(((uint8_t *)ptrs[order])[0] == 0xCC);
			TEST_ASSERT(((uint8_t *)ptrs[order])[size - 1] == 0xCC);
		}


		for (uint32_t order = 0; order <= 4; order++)
			free_page(ptrs[order], order);
	}
	TEST_END("buddy: multi-order alloc/free");
	return __test_ret;
fail:
	TEST_FAIL("buddy: multi-order alloc/free", "see above");

	return __test_ret;
}

int test_buddy_merge(void)
{
	TEST_BEGIN("buddy: buddy merging");
	{

		void *p0 = get_free_page(0, ALLOC_NOWAIT);
		void *p1 = get_free_page(0, ALLOC_NOWAIT);
		void *p2 = get_free_page(0, ALLOC_NOWAIT);
		void *p3 = get_free_page(0, ALLOC_NOWAIT);

		TEST_ASSERT_NOT_NULL(p0);
		TEST_ASSERT_NOT_NULL(p1);
		TEST_ASSERT_NOT_NULL(p2);
		TEST_ASSERT_NOT_NULL(p3);


		free_page(p0, 0);
		free_page(p1, 0);
		free_page(p2, 0);
		free_page(p3, 0);


		void *big = get_free_page(2, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(big);
		TEST_ASSERT_ALIGNED(big, (size_t)PAGE_SIZE << 2);

		free_page(big, 2);
	}
	TEST_END("buddy: buddy merging");
	return __test_ret;
fail:
	TEST_FAIL("buddy: buddy merging", "see above");

	return __test_ret;
}

int test_buddy_stress(void)
{
	TEST_BEGIN("buddy: stress alloc/free cycle");
	{
#define BUDDY_STRESS_N 64
		void *ptrs[BUDDY_STRESS_N];

		for (int round = 0; round < 3; round++) {

			for (int i = 0; i < BUDDY_STRESS_N; i++) {
				ptrs[i] = get_free_page(0, ALLOC_NOWAIT);
				TEST_ASSERT_NOT_NULL(ptrs[i]);
				memset(ptrs[i], (uint8_t)(round + i),
				       PAGE_SIZE);
			}


			for (int i = 0; i < BUDDY_STRESS_N; i++)
				free_page(ptrs[i], 0);
		}
#undef BUDDY_STRESS_N
	}
	TEST_END("buddy: stress alloc/free cycle");
	return __test_ret;
fail:
	TEST_FAIL("buddy: stress alloc/free cycle", "see above");

	return __test_ret;
}

int test_buddy_split(void)
{
	TEST_BEGIN("buddy: order split");
	{

		void *big = get_free_page(3, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(big);


		memset(big, 0xDD, PAGE_SIZE << 3);
		free_page(big, 3);


		void *pages[8];
		for (int i = 0; i < 8; i++) {
			pages[i] = get_free_page(0, ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(pages[i]);
		}

		for (int i = 0; i < 8; i++)
			free_page(pages[i], 0);
	}
	TEST_END("buddy: order split");
	return __test_ret;
fail:
	TEST_FAIL("buddy: order split", "see above");

	return __test_ret;
}

int test_buddy_over_order_preserves_free_count(void)
{
	TEST_BEGIN("buddy: over-order preserves free count");
	{
		size_t free_before = buddy_free_pages();
		void *ptr = get_free_page(MAX_ORDER + 1, ALLOC_NOWAIT);

		TEST_ASSERT_NULL(ptr);
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);
	}
	TEST_END("buddy: over-order preserves free count");
	return __test_ret;
fail:
	TEST_FAIL("buddy: over-order preserves free count", "see above");

	return __test_ret;
}

int test_buddy_multi_order_preserves_free_count(void)
{
	TEST_BEGIN("buddy: multi-order preserves free count");
	{
		void *ptrs[5];
		size_t free_before = buddy_free_pages();

		for (uint32_t order = 0; order <= 4; order++) {
			ptrs[order] = get_free_page(order, ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(ptrs[order]);
		}

		TEST_ASSERT_EQ(buddy_free_pages(),
			       free_before - ((1UL << 5) - 1));

		for (uint32_t order = 0; order <= 4; order++)
			free_page(ptrs[order], order);

		TEST_ASSERT_EQ(buddy_free_pages(), free_before);
	}
	TEST_END("buddy: multi-order preserves free count");
	return __test_ret;
fail:
	TEST_FAIL("buddy: multi-order preserves free count", "see above");

	return __test_ret;
}

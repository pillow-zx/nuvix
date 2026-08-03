#include <kernel/slab.h>
#include <kernel/buddy.h>
#include <kernel/test.h>
#include <kernel/page.h>

#include "../ktest.h"

int test_slab_basic(void)
{
	TEST_BEGIN("slab: basic alloc/free");
	{
#define SLAB_NR_CACHES 8
		const size_t sizes[SLAB_NR_CACHES] = {16,  32,	64,   128,
						      256, 512, 1024, 2048};
		void *ptrs[SLAB_NR_CACHES];


		for (int i = 0; i < SLAB_NR_CACHES; i++) {
			ptrs[i] = kmalloc(sizes[i], ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(ptrs[i]);
			memset(ptrs[i], 0xAA, sizes[i]);
		}


		for (int i = 0; i < SLAB_NR_CACHES; i++)
			kfree(ptrs[i]);


		for (int i = 0; i < SLAB_NR_CACHES; i++) {
			ptrs[i] = kmalloc(sizes[i], ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(ptrs[i]);
		}


		for (int i = 0; i < SLAB_NR_CACHES; i++)
			kfree(ptrs[i]);
#undef SLAB_NR_CACHES
	}
	TEST_END("slab: basic alloc/free");
	return __test_ret;
fail:
	TEST_FAIL("slab: basic alloc/free", "see above");

	return __test_ret;
}
int test_slab_cross_cache(void)
{
	TEST_BEGIN("slab: cross-cache sizes");
	{

		size_t odd_sizes[] = {1,   7,	15,  17,   33,	65,
				      100, 200, 500, 1000, 1500};
		int n = sizeof(odd_sizes) / sizeof(odd_sizes[0]);
		void *ptrs[16];

		TEST_ASSERT(n <= 16);

		for (int i = 0; i < n; i++) {
			ptrs[i] = kmalloc(odd_sizes[i], ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(ptrs[i]);
			memset(ptrs[i], 0xEE, odd_sizes[i]);
		}

		for (int i = 0; i < n; i++)
			kfree(ptrs[i]);
	}
	TEST_END("slab: cross-cache sizes");
	return __test_ret;
fail:
	TEST_FAIL("slab: cross-cache sizes", "see above");

	return __test_ret;
}

int test_slab_stress(void)
{
	TEST_BEGIN("slab: stress cycle");
	{
#define SLAB_STRESS_N 128
		void *ptrs[SLAB_STRESS_N];

		for (int round = 0; round < 5; round++) {
			for (int i = 0; i < SLAB_STRESS_N; i++) {

				size_t sz = 16 << (i % 8);
				ptrs[i] = kmalloc(sz, ALLOC_NOWAIT);
				TEST_ASSERT_NOT_NULL(ptrs[i]);
				memset(ptrs[i], (uint8_t)round, sz);
			}
			for (int i = 0; i < SLAB_STRESS_N; i++)
				kfree(ptrs[i]);
		}
#undef SLAB_STRESS_N
	}
	TEST_END("slab: stress cycle");
	return __test_ret;
fail:
	TEST_FAIL("slab: stress cycle", "see above");

	return __test_ret;
}

int test_slab_returns_empty_page_to_buddy(void)
{
	TEST_BEGIN("slab: returns empty page to buddy");
	{
#define SLAB_RECLAIM_PTRS 512
		void *ptrs[SLAB_RECLAIM_PTRS];
		size_t nr_ptrs = 0;
		size_t free_before;
		size_t free_after_refill;
		size_t free_after_partial;

		free_before = buddy_free_pages();

		while (nr_ptrs < SLAB_RECLAIM_PTRS &&
		       buddy_free_pages() == free_before) {
			ptrs[nr_ptrs] = kmalloc(16, ALLOC_NOWAIT);
			TEST_ASSERT_NOT_NULL(ptrs[nr_ptrs]);
			memset(ptrs[nr_ptrs], 0x5a, 16);
			nr_ptrs++;
		}

		TEST_ASSERT(nr_ptrs > 0);
		TEST_ASSERT(buddy_free_pages() < free_before);
		free_after_refill = buddy_free_pages();

		for (size_t i = 0; i + 1 < nr_ptrs; i++)
			kfree(ptrs[i]);

		free_after_partial = buddy_free_pages();
		TEST_ASSERT_EQ(free_after_partial, free_after_refill);

		kfree(ptrs[nr_ptrs - 1]);
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);

		ptrs[0] = kmalloc(16, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(ptrs[0]);
		kfree(ptrs[0]);
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);
#undef SLAB_RECLAIM_PTRS
	}
	TEST_END("slab: returns empty page to buddy");
	return __test_ret;
fail:
	TEST_FAIL("slab: returns empty page to buddy", "see above");

	return __test_ret;
}

int test_kmalloc_large_alloc_free(void)
{
	TEST_BEGIN("kmalloc: large alloc/free");
	{
		size_t free_before = buddy_free_pages();
		void *ptr = kmalloc(3000, ALLOC_NOWAIT);

		TEST_ASSERT_NOT_NULL(ptr);
		memset(ptr, 0x6b, 3000);
		TEST_ASSERT(((uint8_t *)ptr)[0] == 0x6b);
		TEST_ASSERT(((uint8_t *)ptr)[2999] == 0x6b);
		TEST_ASSERT(buddy_free_pages() < free_before);

		kfree(ptr);
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);
	}
	TEST_END("kmalloc: large alloc/free");
	return __test_ret;
fail:
	TEST_FAIL("kmalloc: large alloc/free", "see above");

	return __test_ret;
}

int test_kzalloc_large_zeroes_requested_size(void)
{
	TEST_BEGIN("kzalloc: large zeroes requested size");
	{
		uint8_t *ptr = kzalloc(3000, ALLOC_NOWAIT);

		TEST_ASSERT_NOT_NULL(ptr);
		for (size_t i = 0; i < 3000; i++)
			TEST_ASSERT(ptr[i] == 0);

		kfree(ptr);
	}
	TEST_END("kzalloc: large zeroes requested size");
	return __test_ret;
fail:
	TEST_FAIL("kzalloc: large zeroes requested size", "see above");

	return __test_ret;
}

int test_kmalloc_oversize_preserves_free_count(void)
{
	TEST_BEGIN("kmalloc: oversize preserves free count");
	{
		size_t free_before = buddy_free_pages();
		void *ptr = kmalloc((size_t)PAGE_SIZE << MAX_ORDER,
				    ALLOC_NOWAIT);

		TEST_ASSERT_NULL(ptr);
		TEST_ASSERT_EQ(buddy_free_pages(), free_before);
	}
	TEST_END("kmalloc: oversize preserves free count");
	return __test_ret;
fail:
	TEST_FAIL("kmalloc: oversize preserves free count", "see above");

	return __test_ret;
}

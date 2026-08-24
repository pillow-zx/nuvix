/* Generic allocation-free introspective sort. */

#include <nuvix/bitops.h>
#include <nuvix/compiler.h>
#include <nuvix/sort.h>

#define SORT_INSERTION_THRESHOLD 16
#define SORT_HOLE_MAX		 64

__always_inline __must_check __pure
static inline unsigned char *sort_element(unsigned char *base, size_t index, size_t size)
{
	return base + index * size;
}

static void sort_swap(unsigned char *restrict lhs, unsigned char *restrict rhs, size_t size)
{
	if (lhs == rhs)
		return;

	if (size % sizeof(uint64_t) == 0 &&
	    !(((uintptr_t)lhs | (uintptr_t)rhs) & (sizeof(uint64_t) - 1))) {
		uint64_t *restrict left = (uint64_t *)lhs;
		uint64_t *restrict right = (uint64_t *)rhs;

		for (size_t words = size / sizeof(*left); words; words--) {
			uint64_t word = *left;

			*left++ = *right;
			*right++ = word;
		}
		return;
	}

	if (size % sizeof(uint32_t) == 0 &&
	    !(((uintptr_t)lhs | (uintptr_t)rhs) & (sizeof(uint32_t) - 1))) {
		uint32_t *restrict left = (uint32_t *)lhs;
		uint32_t *restrict right = (uint32_t *)rhs;

		for (size_t words = size / sizeof(*left); words; words--) {
			uint32_t word = *left;

			*left++ = *right;
			*right++ = word;
		}
		return;
	}

	if (size % sizeof(uint16_t) == 0 &&
	    !(((uintptr_t)lhs | (uintptr_t)rhs) & (sizeof(uint16_t) - 1))) {
		uint16_t *restrict left = (uint16_t *)lhs;
		uint16_t *restrict right = (uint16_t *)rhs;

		for (size_t words = size / sizeof(*left); words; words--) {
			uint16_t word = *left;

			*left++ = *right;
			*right++ = word;
		}
		return;
	}

	while (size--) {
		unsigned char byte = *lhs;

		*lhs++ = *rhs;
		*rhs++ = byte;
	}
}

static void sort_copy(unsigned char *restrict dst, const unsigned char *restrict src, size_t size)
{
	if (size % sizeof(uint64_t) == 0 &&
	    !(((uintptr_t)dst | (uintptr_t)src) & (sizeof(uint64_t) - 1))) {
		uint64_t *restrict to = (uint64_t *)dst;
		const uint64_t *restrict from = (const uint64_t *)src;

		for (size_t words = size / sizeof(*to); words; words--)
			*to++ = *from++;
		return;
	}

	if (size % sizeof(uint32_t) == 0 &&
	    !(((uintptr_t)dst | (uintptr_t)src) & (sizeof(uint32_t) - 1))) {
		uint32_t *restrict to = (uint32_t *)dst;
		const uint32_t *restrict from = (const uint32_t *)src;

		for (size_t words = size / sizeof(*to); words; words--)
			*to++ = *from++;
		return;
	}

	if (size % sizeof(uint16_t) == 0 &&
	    !(((uintptr_t)dst | (uintptr_t)src) & (sizeof(uint16_t) - 1))) {
		uint16_t *restrict to = (uint16_t *)dst;
		const uint16_t *restrict from = (const uint16_t *)src;

		for (size_t words = size / sizeof(*to); words; words--)
			*to++ = *from++;
		return;
	}

	while (size--)
		*dst++ = *src++;
}

static void sort_insertion(unsigned char *base, size_t nr, size_t size, cmp_t compare)
{
	if (nr < 2)
		return;

	unsigned char *current = base + size;
	unsigned char *end = base + nr * size;

	if (size <= SORT_HOLE_MAX) {
		/*
		 * Hole-based insertion: hold the element under insertion in
		 * local storage and shift predecessors instead of swapping.
		 * This halves the memory traffic per moved element and lets
		 * the comparator read the cached copy instead of the array.
		 */
		unsigned char hole[SORT_HOLE_MAX];

		while (current < end) {
			unsigned char *scan = current;

			sort_copy(hole, current, size);
			while (scan != base) {
				unsigned char *previous = scan - size;

				if (compare(previous, hole) <= 0)
					break;
				sort_copy(scan, previous, size);
				scan = previous;
			}
			sort_copy(scan, hole, size);
			current += size;
		}
		return;
	}

	while (current < end) {
		unsigned char *scan = current;

		while (scan != base) {
			unsigned char *previous = scan - size;

			if (compare(previous, scan) <= 0)
				break;
			sort_swap(previous, scan, size);
			scan = previous;
		}
		current += size;
	}
}

static size_t sort_median_of_three(unsigned char *base, size_t nr, size_t size, cmp_t compare)
{
	size_t middle = nr / 2;
	size_t last = nr - 1;

	unsigned char *first_element = sort_element(base, 0, size);
	unsigned char *middle_element = sort_element(base, middle, size);
	unsigned char *last_element = sort_element(base, last, size);

	if (compare(first_element, middle_element) < 0) {
		if (compare(middle_element, last_element) < 0)
			return middle;
		return compare(first_element, last_element) < 0 ? last : 0;
	}

	if (compare(first_element, last_element) < 0)
		return 0;
	return compare(middle_element, last_element) < 0 ? last : middle;
}

static void sort_partition(unsigned char *base, size_t nr, size_t size,
			   cmp_t compare, size_t *less_end,
			   size_t *greater_begin)
{
	size_t pivot_index = sort_median_of_three(base, nr, size, compare);
	sort_swap(sort_element(base, pivot_index, size),
		  sort_element(base, nr - 1, size), size);

	unsigned char *pivot = sort_element(base, nr - 1, size);
	unsigned char *less_element = base;
	unsigned char *current_element = base;
	unsigned char *greater_element = pivot;
	size_t less = 0;
	size_t current = 0;
	size_t greater = nr - 1;

	while (current < greater) {
		int order = compare(current_element, pivot);

		if (order < 0) {
			sort_swap(current_element, less_element, size);
			current_element += size;
			less_element += size;
			current++;
			less++;
		} else if (order > 0) {
			greater--;
			greater_element -= size;
			sort_swap(current_element, greater_element, size);
		} else {
			current_element += size;
			current++;
		}
	}

	sort_swap(greater_element, pivot, size);
	*less_end = less;
	*greater_begin = greater + 1;
}

static void sort_sift_down(unsigned char *base, size_t root, size_t nr,
			   size_t size, cmp_t compare)
{
	while (root < nr / 2) {
		size_t child = root * 2 + 1;

		if (child + 1 < nr &&
		    compare(sort_element(base, child, size),
			    sort_element(base, child + 1, size)) < 0)
			child++;

		if (compare(sort_element(base, root, size),
			    sort_element(base, child, size)) >= 0)
			return;

		sort_swap(sort_element(base, root, size),
			  sort_element(base, child, size), size);
		root = child;
	}
}

static void sort_heap(unsigned char *base, size_t nr, size_t size,
		      cmp_t compare)
{
	size_t root = nr / 2;

	while (root) {
		--root;
		sort_sift_down(base, root, nr, size, compare);
	}

	while (nr > 1) {
		--nr;
		sort_swap(base, sort_element(base, nr, size), size);
		sort_sift_down(base, 0, nr, size, compare);
	}
}

static bool is_sorted(unsigned char *base, size_t nr, size_t size,
		      cmp_t compare)
{
	unsigned char *previous = base;
	unsigned char *current = base + size;

	for (size_t i = 1; i < nr; i++) {
		if (compare(previous, current) > 0)
			return false;
		previous = current;
		current += size;
	}
	return true;
}

static void sort_intro(unsigned char *base, size_t nr, size_t size,
		       cmp_t compare, size_t depth)
{
	while (nr > SORT_INSERTION_THRESHOLD) {
		size_t less;
		size_t greater;

		if (!depth) {
			sort_heap(base, nr, size, compare);
			return;
		}
		--depth;
		sort_partition(base, nr, size, compare, &less, &greater);

		size_t left = less;
		size_t right = nr - greater;

		if (left < right) {
			sort_intro(base, left, size, compare, depth);
			base += greater * size;
			nr = right;
		} else {
			sort_intro(base + greater * size, right, size, compare,
				   depth);
			nr = left;
		}
	}

	sort_insertion(base, nr, size, compare);
}

void sort(void *base, size_t nr, size_t size, cmp_t compare)
{
	if (!base || nr < 2 || size == 0 || !compare)
		return;
	if (nr > SIZE_MAX / size)
		return;

	unsigned char *array = base;

	if (nr > SORT_INSERTION_THRESHOLD &&
	    is_sorted(array, nr, size, compare))
		return;

	sort_intro(array, nr, size, compare, 2 * (fls(nr) - 1));
}

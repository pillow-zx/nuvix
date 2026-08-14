/**
 * @file page.h
 * @brief 物理页描述符和 page flag helpers。
 */

#ifndef _NUVIX_PAGE_H
#define _NUVIX_PAGE_H

#include <nuvix/types.h>
#include <nuvix/list.h>
#include <nuvix/bitops.h>
#include <nuvix/compiler.h>
#include <nuvix/refcount.h>
#include <arch/page.h>

/**
 * @def PG_RESERVED
 * @brief Physical page is reserved and unavailable to general allocators.
 */
#define PG_RESERVED 0

/**
 * @def PG_SLAB
 * @brief Physical page is owned by the slab allocator.
 */
#define PG_SLAB 1

/**
 * @def PG_BUDDY
 * @brief Physical page is currently managed by the buddy allocator.
 */
#define PG_BUDDY 2

/**
 * @struct page
 * @brief Physical page descriptor in the global mem_map array.
 *
 * @par Fields
 * - @c flags: PG_* bitset.
 * - @c order: Buddy order when page is a free block head.
 * - @c refcount: References held by allocators/users.
 * - @c lru: Allocator or cache intrusive list node.
 */
struct page {
	uint32_t flags;
	uint32_t order;
	refcount_t refcount;
	struct list_head lru;
};

__must_check __pure __nonnull(1)
static inline bool page_test_flag(const struct page *page, uint32_t flag)
{
	return test_bit(page->flags, flag);
}

__nonnull(1)
static inline void page_set_flag(struct page *page, uint32_t flag)
{
	set_bit(page->flags, flag);
}

__nonnull(1)
static inline void page_clear_flag(struct page *page,
						uint32_t flag)
{
	clr_bit(page->flags, flag);
}

#endif

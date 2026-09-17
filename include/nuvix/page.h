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

#define PG_RESERVED 0
#define PG_SLAB 1
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

#endif

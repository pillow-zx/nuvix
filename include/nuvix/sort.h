#ifndef _NUVIX_SORT_H
#define _NUVIX_SORT_H

/**
 * @file sort.h
 * @brief In-place generic sorting.
 */

#include <nuvix/types.h>

typedef int (*cmp_t)(const void *lhs, const void *rhs);

/**
 * Sort an array in ascending order according to @p compare.
 *
 * The comparator follows the usual negative/zero/positive convention. The
 * implementation is allocation-free and intended for the small bounded
 * arrays common in kernel metadata paths.
 */
void sort(void *base, size_t nr, size_t size, cmp_t compare);

#endif

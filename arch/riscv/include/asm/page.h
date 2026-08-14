#ifndef _NUVIX_ASM_PAGE_H
#define _NUVIX_ASM_PAGE_H

/**
 * @file page.h
 * @brief RISC-V base page-size and PFN conversion definitions.
 */

#include <nuvix/types.h>

/**
 * @def PAGE_SIZE
 * @brief Base page size used by Sv39 page tables and kernel allocators.
 */
#define PAGE_SIZE  4096

/**
 * @def PAGE_SHIFT
 * @brief log2(PAGE_SIZE), used for byte/PFN conversion.
 */
#define PAGE_SHIFT 12

/**
 * @def PAGE_MASK
 * @brief Address mask that rounds down to a page boundary.
 */
#define PAGE_MASK  (~(PAGE_SIZE - 1))

/** @def PFN_DOWN Convert byte address to containing page frame number. */
#define PFN_DOWN(x)    ((x) >> PAGE_SHIFT)
/** @def PFN_UP Convert byte size/address to rounded-up page frame count. */
#define PFN_UP(x)      (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
/** @def PFN_PHYS Convert a page frame number to a physical byte address. */
#define PFN_PHYS(pfn)  ((uint64_t)(pfn) << PAGE_SHIFT)
/** @def PHYS_PFN Convert a physical byte address to a page frame number. */
#define PHYS_PFN(addr) ((addr) >> PAGE_SHIFT)

#endif

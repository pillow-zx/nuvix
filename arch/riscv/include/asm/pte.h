#ifndef _NUVIX_ASM_PTE_H
#define _NUVIX_ASM_PTE_H

/**
 * @file
 * @brief RISC-V Sv39 page-table entry bit definitions.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/bitops.h>
#include <asm/page.h>

/**
 * @typedef pte_t
 * @brief Raw 64-bit Sv39 page-table entry.
 *
 * Bits 9:0 hold architectural flags and bits 53:10 hold the physical page
 * number. nuvix only emits Sv39 leaf and table entries; reserved bits must
 * stay zero.
 */
typedef uint64_t pte_t;

/** @def PTE_V
 * @brief Entry is valid. Invalid entries are ignored by hardware walks.
 */
#define PTE_V BIT(0)
/** @def PTE_R
 * @brief Leaf permits loads from the mapped virtual page.
 */
#define PTE_R BIT(1)
/** @def PTE_W
 * @brief Leaf permits stores. Sv39 requires writable leaves to be readable.
 */
#define PTE_W BIT(2)
/** @def PTE_X
 * @brief Leaf permits instruction fetch.
 */
#define PTE_X BIT(3)
/** @def PTE_U
 * @brief Leaf is accessible from U-mode when status allows the access.
 */
#define PTE_U BIT(4)
/** @def PTE_G
 * @brief Global mapping, shared across address spaces for TLB purposes.
 */
#define PTE_G BIT(5)
/** @def PTE_A
 * @brief Accessed bit. nuvix sets it eagerly and does not handle A faults.
 */
#define PTE_A BIT(6)
/** @def PTE_D
 * @brief Dirty bit. nuvix sets it eagerly and does not handle D faults.
 */
#define PTE_D BIT(7)

/** @def PTE_TABLE
 * @brief Non-leaf page-table pointer: valid with R/W/X all clear.
 */
#define PTE_TABLE PTE_V

/** @def PTE_KERN_R
 * @brief Kernel global read-only leaf permissions.
 */
#define PTE_KERN_R   (PTE_V | PTE_R | PTE_G | PTE_A | PTE_D)
/** @def PTE_KERN_RX
 * @brief Kernel global executable leaf permissions.
 */
#define PTE_KERN_RX  (PTE_V | PTE_X | PTE_G | PTE_A | PTE_D)
/** @def PTE_KERN_RW
 * @brief Kernel global writable data leaf permissions.
 */
#define PTE_KERN_RW  (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)
/** @def PTE_KERN_RWX
 * @brief Kernel global read/write/execute leaf permissions.
 */
#define PTE_KERN_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)

/** @def PTE_USER_R
 * @brief User read-only leaf permissions.
 */
#define PTE_USER_R   (PTE_V | PTE_R | PTE_U | PTE_A | PTE_D)
/** @def PTE_USER_RX
 * @brief User readable executable leaf permissions.
 */
#define PTE_USER_RX  (PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
/** @def PTE_USER_RW
 * @brief User readable/writable leaf permissions.
 */
#define PTE_USER_RW  (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)
/** @def PTE_USER_RWX
 * @brief User readable/writable/executable leaf permissions.
 */
#define PTE_USER_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)

/** @def PTE_PPN_SHIFT
 * @brief Bit offset of the Sv39 physical page number inside a PTE.
 */
#define PTE_PPN_SHIFT	       10
/** @def PTE_PPN
 * @brief Extract a physical page number from a raw PTE.
 * @param pte Raw Sv39 page-table entry.
 */
#define PTE_PPN(pte)	       ((pte) >> PTE_PPN_SHIFT)
/** @def PTE_CREATE
 * @brief Combine a physical frame number and permission flags into a PTE.
 * @param pfn Physical frame number, not a byte address.
 * @param flags Sv39 flag bits such as PTE_V/PTE_R/PTE_W/PTE_X.
 */
#define PTE_CREATE(pfn, flags) (((uint64_t)(pfn) << PTE_PPN_SHIFT) | (flags))
/** @def PTE_TO_PA
 * @brief Convert a PTE's physical page number into a byte physical address.
 * @param pte Raw Sv39 page-table entry.
 */
#define PTE_TO_PA(pte)	       ((uint64_t)PTE_PPN(pte) << PAGE_SHIFT)
/** @def PA_TO_PTE
 * @brief Convert a page-aligned physical address into a flagless PTE payload.
 * @param pa Page-aligned physical byte address.
 */
#define PA_TO_PTE(pa)	       (PTE_CREATE(PHYS_PFN(pa), 0))

/**
 * @brief Test whether a PTE is valid.
 * @param pte Raw Sv39 page-table entry.
 * @return True when PTE_V is set.
 */
__always_inline __must_check __pure
static inline bool asm_pte_present(pte_t pte)
{
	return (pte & PTE_V) != 0;
}

/**
 * @brief Test whether a PTE maps a user-accessible physical page.
 * @param pte Raw Sv39 page-table entry.
 * @return True for a user leaf with a non-zero physical page number.
 */
__always_inline __must_check __pure
static inline bool asm_pte_user_page(pte_t pte)
{
	return (pte & PTE_U) != 0 && PTE_TO_PA(pte) != 0;
}

/**
 * @brief Test whether a PTE is a leaf rather than a next-level page table.
 * @param pte Raw Sv39 page-table entry.
 * @return True when any of R/W/X is set.
 */
__always_inline __must_check __pure
static inline bool asm_pte_leaf(pte_t pte)
{
	return (pte & (PTE_R | PTE_W | PTE_X)) != 0;
}

/**
 * @brief Return the physical base address encoded in a PTE.
 * @param pte Raw Sv39 page-table entry.
 * @return Page-aligned physical byte address.
 */
__always_inline __must_check __pure
static inline paddr_t asm_pte_to_pa(pte_t pte)
{
	return PTE_TO_PA(pte);
}

#endif

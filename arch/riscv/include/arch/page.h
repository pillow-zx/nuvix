#ifndef _NUVIX_ARCH_RISCV_PAGE_H
#define _NUVIX_ARCH_RISCV_PAGE_H

/**
 * @file page.h
 * @brief RISC-V platform address layout exported to generic MM.
 */

#include <asm/page.h>
#include <nuvix/compiler.h>
#include <nuvix/types.h>

/**
 * @def DRAM_BASE
 * @brief Physical base address of QEMU virt DRAM.
 */
#define DRAM_BASE 0x80000000

/**
 * @def DRAM_SIZE
 * @brief Configured DRAM size in bytes.
 */
#define DRAM_SIZE ((uint64_t)CONFIG_DRAM_SIZE_MB << 20)

/**
 * @def TASK_SIZE
 * @brief Highest user virtual address plus one for the current Sv39 policy.
 */
#define TASK_SIZE 0x80000000UL

/**
 * @def USER_STACK_TOP
 * @brief Exclusive top of the initial user stack region.
 */
#define USER_STACK_TOP TASK_SIZE

/**
 * @def USER_STACK_BASE
 * @brief Size of the fixed initial user stack mapping.
 */
#define USER_STACK_SIZE (64 * 1024)

static_assert(USER_STACK_SIZE % PAGE_SIZE == 0,
	      "user stack size must be page aligned");

/**
 * @def USER_STACK_BASE
 * @brief Base of the fixed initial user stack mapping.
 */
#define USER_STACK_BASE (USER_STACK_TOP - USER_STACK_SIZE)

/**
 * @def USER_STACK_GUARD_BASE
 * @brief Base of the guard page below the initial user stack.
 */
#define USER_STACK_GUARD_BASE (USER_STACK_BASE - PAGE_SIZE)

/**
 * @def KERNEL_VBASE
 * @brief High-half virtual base of the kernel direct map.
 */
#define KERNEL_VBASE 0xFFFFFFC000000000

/**
 * @def __pa
 * @brief Convert a high-half direct-map kernel virtual address to physical.
 */
#define __pa(x) ((vaddr_t)(uintptr_t)(x) - KERNEL_VBASE)

/**
 * @def __va
 * @brief Convert a physical address to the high-half direct-map address.
 */
#define __va(x) ((void *)((paddr_t)(x) + KERNEL_VBASE))

/**
 * @brief Build and activate the kernel page-table baseline.
 */
void pagetable_init(void);

/**
 * @brief Return the first physical address available after boot allocations.
 */
__must_check
void *bootmem_end(void);

#endif

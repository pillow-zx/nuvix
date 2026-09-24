#ifndef _NUVIX_ARCH_RISCV_CONFIG_H
#define _NUVIX_ARCH_RISCV_CONFIG_H

#include <asm/layout.h>

#define PAGE_SIZE               4096UL
#define PAGE_SHIFT              12
#define PAGE_MASK               (~(PAGE_SIZE - 1UL))

/* Fixed load contract; RAM outside this early window is mapped in C. */
#define DIRECT_MAP_LIMIT        (128UL << 30)
#define VMALLOC_BASE            0xFFFFFFE000000000UL
#define MMIO_VBASE              0xFFFFFFFE00000000UL
#define MMIO_SIZE               (256UL << 20)
#define DTB_VBASE               0xFFFFFFFF00000000UL
#define DTB_MAX_SIZE            (16UL << 20)

#define TASK_SIZE	        0x80000000UL
#define USER_STACK_TOP	        TASK_SIZE
#define USER_STACK_SIZE	        (4UL * 1024)
#define USER_STACK_BASE	        (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_GUARD_BASE   (USER_STACK_BASE - PAGE_SIZE)

#ifdef CONFIG_SMP
#define NR_CPUS		        CONFIG_MAX_CPUS
#else
#define NR_CPUS                  1
#endif

#endif

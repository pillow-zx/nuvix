#ifndef _NUVIX_RISCV_LAYOUT_H
#define _NUVIX_RISCV_LAYOUT_H

/* Shared by C, assembly and the preprocessed linker script. No C suffixes. */
#define KERNEL_VBASE       0xFFFFFFC000000000
#define KERNEL_LOAD_PA     0x80200000
#define BOOT_RAM_BASE      0x80000000
#define BOOT_RAM_END       0xC0000000
#define ARCH_KSTACK_ORDER  3
#define ARCH_KSTACK_SIZE   (4096 << ARCH_KSTACK_ORDER)

#endif

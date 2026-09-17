#ifndef _NUVIX_ARCH_RISCV_CONFIG_H
#define _NUVIX_ARCH_RISCV_CONFIG_H

#define PAGE_SIZE               4096UL
#define PAGE_SHIFT              12
#define PAGE_MASK               (~(PAGE_SIZE - 1UL))

#define DRAM_BASE               0x80000000UL
#define DRAM_SIZE               ((unsigned long)CONFIG_DRAM_SIZE_MB << 20)
#define KERNEL_VBASE            0xFFFFFFC000000000UL

#define TASK_SIZE	        0x80000000UL
#define USER_STACK_TOP	        TASK_SIZE
#define USER_STACK_SIZE	        (4UL * 1024)
#define USER_STACK_BASE	        (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_GUARD_BASE   (USER_STACK_BASE - PAGE_SIZE)

#define ARCH_KSTACK_ORDER       3
#define ARCH_KSTACK_SIZE        (PAGE_SIZE << ARCH_KSTACK_ORDER)
#ifdef CONFIG_SMP
#define NR_CPUS		        CONFIG_QEMU_CPUS
#else
#define NR_CPUS                  1
#endif
#define QEMU_VIRT_MAX_CPUS      8

#define MTIME_FREQ              10000000ULL

#define UART_BASE	        0x10000000UL
#define VIRTIO_MMIO_BASE        0x10001000UL

#endif

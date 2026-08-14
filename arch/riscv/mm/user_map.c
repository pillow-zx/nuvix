/*
 * arch/riscv/mm/user_map.c - QEMU virt 用户页表平台映射
 */

#include <nuvix/printk.h>
#include <nuvix/user_map.h>
#include <arch/page.h>
#include <arch/pgtable.h>
#include <arch/user_map.h>
#include <drivers/uart.h>
#include <drivers/virtio.h>

#define RISCV_MMIO_USER_START UART_BASE
#define RISCV_MMIO_USER_END   (VIRTIO_MMIO_BASE + PAGE_SIZE)

static int riscv_user_mmio_map(pte_t *root)
{
	int ret;

	ret = map_page(root, UART_BASE, UART_BASE, PTE_KERN_RW);
	if (ret < 0)
		return ret;
	return map_page(root, VIRTIO_MMIO_BASE, VIRTIO_MMIO_BASE, PTE_KERN_RW);
}

void user_map_init(void)
{
	int ret;

	ret = user_map_register_reserved("riscv_mmio", RISCV_MMIO_USER_START,
					 RISCV_MMIO_USER_END,
					 riscv_user_mmio_map);
	BUG_ON(ret < 0);
}

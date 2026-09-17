
#include <nuvix/errno.h>
#include <arch/config.h>
#include <arch/page.h>
#include <arch/pgtable.h>
#include <drivers/uart.h>
#include <drivers/virtio.h>

#define MMIO_USER_START UART_BASE
#define MMIO_USER_END	(VIRTIO_MMIO_BASE + PAGE_SIZE)

int arch_upgd_region(vaddr_t *start, vaddr_t *end)
{
	if (!start || !end)
		return -EINVAL;

	*start = MMIO_USER_START;
	*end = MMIO_USER_END;
	return 0;
}

int arch_upgd_init(pte_t *root)
{
	int ret;

	if (!root)
		return -EINVAL;

	ret = map_page(root, UART_BASE, UART_BASE, PTE_KERN_RW);
	if (ret < 0)
		return ret;
	return map_page(root, VIRTIO_MMIO_BASE, VIRTIO_MMIO_BASE, PTE_KERN_RW);
}

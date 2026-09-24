#include <nuvix/bootdev.h>
#include <nuvix/dt.h>
#include <nuvix/printk.h>
#include <drivers/uart.h>
#include <drivers/virtio_blk.h>

/* Small explicit match tables, independent of the selected board. */
static const struct console_driver {
	const char *compatible;
	void (*init)(int node, uint32_t baud);
	int (*start)(void);
} consoles[] = {
	{ "ns16550a", uart_init, uart_irq_start },
	{ "ns16550", uart_init, uart_irq_start },
};

static const struct disk_driver {
	const char *compatible;
	vaddr_t (*probe)(int node);
	dev_t (*init)(vaddr_t base, unsigned irq);
} disks[] = {
	{ "virtio,mmio", virtio_blk_probe, virtio_blk_init },
};

static const struct disk_driver *root_driver;
static vaddr_t root_base;
static unsigned root_irq;
static const struct console_driver *boot_console;

static const struct console_driver *console_match(int node)
{
	for (unsigned i = 0; i < sizeof(consoles) / sizeof(consoles[0]); i++)
		if (!fdt_node_check_compatible(dt_blob, node, consoles[i].compatible))
			return &consoles[i];
	return NULL;
}

void boot_devices_prepare(void)
{
	uint32_t baud;
	int console_node = dt_stdout(&baud), node = -1;
	const struct console_driver *console = NULL;
	if (console_node >= 0) {
		console = console_match(console_node);
		if (!console)
			panic("console: stdout-path has no supported driver");
	} else {
		while ((node = fdt_next_node(dt_blob, node, NULL)) >= 0) {
			const struct console_driver *match = console_match(node);
			if (!match || !dt_available(node))
				continue;
			if (console)
				panic("console: multiple UARTs require stdout-path");
			console = match;
			console_node = node;
		}
		if (!console)
			panic("console: no supported UART");
	}
	console->init(console_node, baud);
	boot_console = console;
	pr_info("console: %s\n", fdt_get_name(dt_blob, console_node, NULL));
	node = -1;
	while ((node = fdt_next_node(dt_blob, node, NULL)) >= 0) {
		if (!dt_available(node))
			continue;
		for (unsigned i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
			if (fdt_node_check_compatible(dt_blob, node, disks[i].compatible))
				continue;
			vaddr_t base = disks[i].probe(node);
			if (base) {
				if (root_driver)
					panic("block: multiple boot disks are not supported");
				root_driver = &disks[i];
				root_base = base;
				int irq = dt_irq(node, 0);
				if (irq < 0)
					panic("block: invalid IRQ (%d)", irq);
				root_irq = (unsigned)irq;
				pr_info("block: selected %s\n", fdt_get_name(dt_blob, node, NULL));
			}
			break;
		}
	}
	if (!root_driver)
		panic("block: no supported boot disk");
}

int boot_console_start(void)
{
	BUG_ON(!boot_console);
	return boot_console->start();
}

dev_t boot_disk_init(void)
{
	if (!root_driver)
		panic("block: devices were not prepared");
	return root_driver->init(root_base, root_irq);
}

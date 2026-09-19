/*
 * drivers/uart.c - NS16550A UART 驱动
 */

#include <drivers/uart.h>
#include <nuvix/tools.h>
#include <nuvix/dt.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <arch/pgtable.h>

static vaddr_t uart_base;
static uint32_t reg_shift, io_width;

static inline void uart_write_reg(int reg, uint8_t val)
{
	vaddr_t address = uart_base + ((unsigned)reg << reg_shift);
	if (io_width == 4)
		MMIO_WRITE(uint32_t, address, val);
	else
		MMIO_WRITE(uint8_t, address, val);
}

__must_check
static inline uint8_t uart_read_reg(int reg)
{
	vaddr_t address = uart_base + ((unsigned)reg << reg_shift);
	return io_width == 4 ? (uint8_t)MMIO_READ(uint32_t, address) :
		MMIO_READ(uint8_t, address);
}

static uint32_t uart_property(int node, const char *name, uint32_t fallback)
{
	uint32_t value;
	int ret = dt_u32(node, name, &value);
	if (ret && ret != -ENOENT)
		panic("uart: malformed %s", name);
	return ret ? fallback : value;
}

void uart_init(int node, uint32_t baud)
{
	struct dt_resource resource;
	dt_require_simple_device(node, false);
	if (dt_reg(node, 0, &resource))
		panic("uart: invalid reg");
	reg_shift = uart_property(node, "reg-shift", 0);
	io_width = uart_property(node, "reg-io-width", 1);
	uint32_t offset = uart_property(node, "reg-offset", 0);
	uint32_t clock = uart_property(node, "clock-frequency", 0);
	if (!baud)
		baud = uart_property(node, "current-speed", 115200);
	if (reg_shift > 4 || (io_width != 1 && io_width != 4) ||
	    (io_width == 4 && (reg_shift < 2 || ((resource.start + offset) & 3))) ||
	    fdt_getprop(dt_blob, node, "big-endian", NULL) ||
	    offset > resource.size || ((7UL << reg_shift) + io_width) > resource.size - offset)
		panic("uart: unsupported register layout");
	if (!clock || !baud)
		panic("uart: clock-frequency and nonzero baud are required");
	uint64_t divisor = ((uint64_t)clock + (uint64_t)baud * 8) / ((uint64_t)baud * 16);
	if (!divisor || divisor > 65535)
		panic("uart: baud divisor out of range");
	uart_base = mmio_map(resource.start, resource.size) + offset;
	uart_write_reg(UART_IER, 0x00);


	uart_write_reg(UART_LCR, UART_LCR_DLAB);


	uart_write_reg(0, divisor & 0xff);
	uart_write_reg(1, divisor >> 8);


	uart_write_reg(UART_LCR, UART_LCR_8N1);


	uart_write_reg(UART_FCR, UART_FCR_EN | UART_FCR_CLR);


	uart_write_reg(UART_MCR, 0x00);
}

void uart_putchar(int ch)
{
	while (!(uart_read_reg(UART_LSR) & UART_LSR_THRE))
		;
	uart_write_reg(UART_THR, (uint8_t)ch);
}

int uart_try_getchar(void)
{
	if (!(uart_read_reg(UART_LSR) & UART_LSR_DR))
		return -1;
	return uart_read_reg(UART_RBR);
}

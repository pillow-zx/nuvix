/*
 * drivers/uart.c - NS16550A UART 驱动
 */

#include <drivers/uart.h>
#include <nuvix/tools.h>

__always_inline
static inline void uart_write_reg(int reg, uint8_t val)
{
	MMIO_WRITE(uint8_t, UART_BASE + reg, val);
}

__always_inline __must_check
static inline uint8_t uart_read_reg(int reg)
{
	return MMIO_READ(uint8_t, UART_BASE + reg);
}

void uart_init(void)
{

	uart_write_reg(UART_IER, 0x00);


	uart_write_reg(UART_LCR, UART_LCR_DLAB);


	uart_write_reg(0, 0x01);
	uart_write_reg(1, 0x00);


	uart_write_reg(UART_LCR, UART_LCR_8N1);


	uart_write_reg(UART_FCR, UART_FCR_EN | UART_FCR_CLR);


	uart_write_reg(UART_MCR, 0x00);
}

void uart_putc(int ch)
{
	while (!(uart_read_reg(UART_LSR) & UART_LSR_THRE))
		;
	uart_write_reg(UART_THR, (uint8_t)ch);
}

int uart_try_getc(void)
{
	if (!(uart_read_reg(UART_LSR) & UART_LSR_DR))
		return -1;
	return uart_read_reg(UART_RBR);
}

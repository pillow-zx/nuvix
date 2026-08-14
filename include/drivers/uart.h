/*
 * include/drivers/uart.h - NS16550A UART 驱动接口
 */

#ifndef _NUVIX_DRIVERS_UART_H
#define _NUVIX_DRIVERS_UART_H

#include <nuvix/types.h>

#define UART_BASE	0x10000000UL

#define UART_THR	0
#define UART_RBR	0
#define UART_IER	1
#define UART_FCR	2
#define UART_LCR	3
#define UART_MCR	4
#define UART_LSR	5

#define UART_LSR_DR	0x01
#define UART_LSR_THRE	0x20

#define UART_LCR_DLAB	0x80
#define UART_LCR_8N1	0x03

#define UART_FCR_EN	0x01
#define UART_FCR_CLR	0x06

void uart_init(void);
void uart_putc(int ch);
int uart_try_getc(void);

#endif

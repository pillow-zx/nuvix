/*
 * include/drivers/uart.h - NS16550A UART 驱动接口
 */

#ifndef _NUVIX_DRIVERS_UART_H
#define _NUVIX_DRIVERS_UART_H

#include <nuvix/config.h>
#include <nuvix/types.h>

#define UART_THR	0
#define UART_RBR	0
#define UART_IER	1
#define UART_FCR	2
#define UART_IIR	2
#define UART_LCR	3
#define UART_MCR	4
#define UART_LSR	5
#define UART_MSR	6

#define UART_IER_RX	0x01
#define UART_IER_TX	0x02
#define UART_IER_LINE	0x04
#define UART_IIR_NONE	0x01
#define UART_IIR_ID	0x0e
#define UART_IIR_TX	0x02
#define UART_IIR_RX	0x04
#define UART_IIR_LINE	0x06
#define UART_IIR_TIMEOUT	0x0c
#define UART_MCR_OUT2	0x08

#define UART_LSR_DR	0x01
#define UART_LSR_THRE	0x20
#define UART_LSR_ERRORS	0x1e

#define UART_LCR_DLAB	0x80
#define UART_LCR_8N1	0x03

#define UART_FCR_EN	0x01
#define UART_FCR_CLR	0x06

void uart_init(int node, uint32_t baud);
int uart_irq_start(void);
/* Early boot and panic only; no scheduler or driver lock dependency. */
void uart_poll_putchar(int ch);
void uart_panic_enter(void);
/* RX consumes the software FIFO, never waits for hardware. */
int uart_try_getchar(void);
struct wait_scope;
struct poll_table;
/* Return 1 if RX is ready, 0 if a waiter was armed, or negative errno. */
int uart_rx_prepare(struct wait_scope *scope);
/* Counts input bytes; optional LF->CRLF expansion is queued atomically. */
ssize_t uart_try_write(const char *buf, size_t count, bool crlf);
int uart_tx_poll(struct poll_table *wait, bool crlf);
/* Task-only, uninterruptible write of the entire buffer (logs and echo). */
int uart_write(const char *buf, size_t count, bool crlf);

#endif

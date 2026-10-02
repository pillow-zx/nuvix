#ifndef _NUVIX_PRINTK_H
#define _NUVIX_PRINTK_H

#include <nuvix/compiler.h>
#include <nuvix/stdarg.h>
#include <nuvix/macro.h>

#ifdef CONFIG_UART
void uart_init(void);
int uart_putchar(int c);
int uart_getchar(void);
#else
#include <nuvix/arch/riscv/sbi.h>
#endif

static inline int putchar(int c)
{
    IFDEF(CONFIG_UART, return uart_putchar(c));
    IFNDEF(CONFIG_UART, return sbi_putchar(c));
}

static inline int getchar(void)
{
    IFDEF(CONFIG_UART, return uart_getchar());
    IFNDEF(
        CONFIG_UART, int c; do { c = sbi_getchar(); } while (c < 0); return c);
}

int vsprintf(char *buf, const char *fmt, va_list args) __printf(2, 0);
int printk(const char *fmt, ...) __printf(1, 2);

#endif /* ifndef _NUVIX_PRINTK_H */

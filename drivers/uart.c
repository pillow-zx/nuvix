#include <asm/io.h>
#include <asm/layout.h>
#include <nuvix/printk.h>

#define UART_RX            0
#define UART_TX            0
#define UART_DLL           0
#define UART_DLM           1
#define UART_IER           1
#define UART_FCR           2
#define UART_LCR           3
#define UART_MCR           4
#define UART_LSR           5

#define UART_FCR_ENABLE    (1 << 0)
#define UART_FCR_CLEAR_RX  (1 << 1)
#define UART_FCR_CLEAR_TX  (1 << 2)
#define UART_LCR_8N1       3
#define UART_LCR_DLAB      (1 << 7)
#define UART_MCR_DTR       (1 << 0)
#define UART_MCR_RTS       (1 << 1)
#define UART_LSR_DR        (1 << 0)
#define UART_LSR_THRE      (1 << 5)

#define UART_BAUD          115200
#define UART_CLOCK_DIVISOR 16

static inline uint8_t uart_read(unsigned int reg)
{
    return readb((const volatile void *)(uintptr_t)(UART_VBASE + reg));
}

static inline void uart_write(unsigned int reg, uint8_t value)
{
    writeb(value, (volatile void *)(uintptr_t)(UART_VBASE + reg));
}

void uart_init(void)
{
    unsigned int divisor = UART_CLOCK / (UART_CLOCK_DIVISOR * UART_BAUD);

    /* Clear DLAB before accessing IER; polling needs no interrupts. */
    uart_write(UART_LCR, UART_LCR_8N1);
    uart_write(UART_IER, 0);
    uart_write(UART_LCR, UART_LCR_8N1 | UART_LCR_DLAB);
    uart_write(UART_DLL, divisor & 0xff);
    uart_write(UART_DLM, divisor >> 8);
    uart_write(UART_LCR, UART_LCR_8N1);
    uart_write(UART_FCR,
               UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
    uart_write(UART_MCR, UART_MCR_DTR | UART_MCR_RTS);

    putchar = uart_putchar;
    getchar = uart_getchar;
}

int uart_putchar(int c)
{
    while (!(uart_read(UART_LSR) & UART_LSR_THRE))
        ;
    uart_write(UART_TX, (uint8_t)c);
    return (uint8_t)c;
}

int uart_getchar(void)
{
    while (!(uart_read(UART_LSR) & UART_LSR_DR))
        ;
    return uart_read(UART_RX);
}

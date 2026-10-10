#include <asm/io.h>
#include <asm/layout.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/kfifo.h>
#include <nuvix/platform.h>
#include <nuvix/printk.h>
#include <nuvix/wait.h>

#define UART_RX           0
#define UART_TX           0
#define UART_DLL          0
#define UART_DLM          1
#define UART_IER          1
#define UART_IIR          2
#define UART_FCR          2
#define UART_LCR          3
#define UART_MCR          4
#define UART_LSR          5
#define UART_MSR          6

#define UART_IER_RDI      (1 << 0)
#define UART_IER_RLSI     (1 << 2)
#define UART_IIR_NO_INT   (1 << 0)
#define UART_IIR_ID       0x0e
#define UART_IIR_RDI      0x04
#define UART_IIR_RLSI     0x06
#define UART_IIR_TIMEOUT  0x0c
#define UART_FCR_ENABLE   (1 << 0)
#define UART_FCR_CLEAR_RX (1 << 1)
#define UART_FCR_CLEAR_TX (1 << 2)
#define UART_LCR_8N1      3
#define UART_LCR_DLAB     (1 << 7)
#define UART_MCR_DTR      (1 << 0)
#define UART_MCR_RTS      (1 << 1)
#define UART_MCR_OUT2     (1 << 3)
#define UART_LSR_DR       (1 << 0)
#define UART_LSR_OE       (1 << 1)
#define UART_LSR_PE       (1 << 2)
#define UART_LSR_FE       (1 << 3)
#define UART_LSR_BI       (1 << 4)
#define UART_LSR_THRE     (1 << 5)
#define UART_LSR_ERRORS (UART_LSR_OE | UART_LSR_PE | UART_LSR_FE | UART_LSR_BI)

#define UART_BAUD       115200
#define UART_CLOCK_DIVISOR 16
#define UART_RX_FIFO_SIZE  256
#define UART_IRQ_BUDGET    256

static struct {
    /* wait.lock protects the FIFO and overflow count. */
    struct wait_queue wait;
    struct kfifo fifo;
    uint8_t buffer[UART_RX_FIFO_SIZE];
    uint64_t rx_overrun;
    uint64_t lsr_errors;
} uart_rx;

static int uart_putchar(int c);
static int uart_getchar(void);

static inline uint8_t uart_read(unsigned int reg)
{
    return readb((const volatile void *)(uintptr_t)(UART_VBASE + reg));
}

static inline void uart_write(unsigned int reg, uint8_t value)
{
    writeb(value, (volatile void *)(uintptr_t)(UART_VBASE + reg));
}

static uint8_t uart_read_lsr(void)
{
    uint8_t lsr = uart_read(UART_LSR);

    /* Polling output also reads and clears receiver error indications. */
    if (lsr & UART_LSR_ERRORS)
        __atomic_fetch_add(&uart_rx.lsr_errors, 1, __ATOMIC_RELAXED);
    return lsr;
}

static void uart_irq(void *data)
{
    unsigned long flags;
    unsigned int budget = UART_IRQ_BUDGET;
    bool queued = false;

    (void)data;
    spin_lock_irqsave(&uart_rx.wait.lock, flags);
    while (budget) {
        uint8_t iir = uart_read(UART_IIR);

        if (iir & UART_IIR_NO_INT)
            break;
        --budget;
        switch (iir & UART_IIR_ID) {
        case UART_IIR_RDI:
        case UART_IIR_RLSI:
        case UART_IIR_TIMEOUT:
            while (budget && (uart_read_lsr() & UART_LSR_DR)) {
                uint8_t c = uart_read(UART_RX);

                --budget;
                if (kfifo_put(&uart_rx.fifo, c))
                    queued = true;
                else
                    ++uart_rx.rx_overrun;
            }
            break;
        default:
            Assert(false, "unexpected UART interrupt %u\n", (unsigned int)iir);
        }
    }
    spin_unlock_irqrestore(&uart_rx.wait.lock, flags);
    if (queued)
        wake_up_all(&uart_rx.wait);
}

static int uart_probe(struct device *dev)
{
    const struct platform_device *pdev = to_platform_device(dev);
    const struct resource *mem = platform_get_resource(pdev, RES_MEM, 0);
    const struct resource *irq_res = platform_get_resource(pdev, RES_IRQ, 0);
    unsigned int divisor = UART_CLOCK / (UART_CLOCK_DIVISOR * UART_BAUD);
    unsigned int irq;
    int ret;

    if (!mem || !irq_res)
        return -EINVAL;
    if (pdev->nres != 2 || mem->start != UART_PBASE || mem->size <= UART_MSR ||
        mem->size > PAGE_SIZE)
        return -EOPNOTSUPP;
    irq = irq_res->start;
    wait_queue_init(&uart_rx.wait);
    ret = kfifo_init(&uart_rx.fifo, uart_rx.buffer, sizeof(uart_rx.buffer));
    if (ret)
        return ret;

    /* Clear DLAB before accessing IER and keep RX masked until IRQ setup succeeds. */
    uart_write(UART_LCR, UART_LCR_8N1);
    uart_write(UART_IER, 0);
    uart_write(UART_LCR, UART_LCR_8N1 | UART_LCR_DLAB);
    uart_write(UART_DLL, divisor & 0xff);
    uart_write(UART_DLM, divisor >> 8);
    uart_write(UART_LCR, UART_LCR_8N1);
    uart_write(UART_FCR,
               UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
    uart_write(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
    (void)uart_read_lsr();
    (void)uart_read(UART_IIR);
    (void)uart_read(UART_MSR);

    ret = irq_register(irq, uart_irq, NULL);
    if (ret)
        return ret;
    ret = irq_enable(irq);
    if (ret) {
        Assert(!irq_unregister(irq), "UART IRQ unregister failed\n");
        return ret;
    }
    uart_write(UART_IER, UART_IER_RDI | UART_IER_RLSI);

    dev->drvdata = &uart_rx;
    putchar = uart_putchar;
    getchar = uart_getchar;
    return 0;
}

static int uart_putchar(int c)
{
    while (!(uart_read_lsr() & UART_LSR_THRE))
        ;
    uart_write(UART_TX, (uint8_t)c);
    return (uint8_t)c;
}

static int uart_getchar(void)
{
    uint8_t c;

    wait_event(&uart_rx.wait, kfifo_get(&uart_rx.fifo, &c));
    return c;
}

static const struct of_device_id uart_ids[] = {
    {.compatible = "ns16550a"},
    {NULL},
};

static struct platform_driver uart_driver = {
    .drv =
        {
            .name = "uart",
            .bus = &platform_bus,
            .probe = uart_probe,
        },
    .ids = uart_ids,
};

int uart_register(void)
{
    return driver_register(&uart_driver.drv);
}

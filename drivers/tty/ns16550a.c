#include "uart.h"

#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/kfifo.h>
#include <nuvix/layout.h>
#include <nuvix/mutex.h>
#include <nuvix/pgtable.h>
#include <nuvix/platform.h>
#include <nuvix/slab.h>
#include <nuvix/tty.h>
#include <nuvix/wait.h>
#include <asm/io.h>

#define UART_RX             0
#define UART_TX             0
#define UART_DLL            0
#define UART_DLM            1
#define UART_IER            1
#define UART_IIR            2
#define UART_FCR            2
#define UART_LCR            3
#define UART_MCR            4
#define UART_LSR            5
#define UART_MSR            6

#define UART_IER_RDI        (1 << 0)
#define UART_IER_THRI       (1 << 1)
#define UART_IER_RLSI       (1 << 2)
#define UART_IER_RX         (UART_IER_RDI | UART_IER_RLSI)
#define UART_IIR_NO_INT     (1 << 0)
#define UART_IIR_ID         0x0e
#define UART_IIR_THRI       0x02
#define UART_IIR_RDI        0x04
#define UART_IIR_RLSI       0x06
#define UART_IIR_TIMEOUT    0x0c
#define UART_FCR_ENABLE     (1 << 0)
#define UART_FCR_CLEAR_RX   (1 << 1)
#define UART_FCR_CLEAR_TX   (1 << 2)
#define UART_FCR_TRIGGER_14 (3 << 6)
#define UART_FCR_CONFIG     (UART_FCR_ENABLE | UART_FCR_TRIGGER_14)
#define UART_LCR_8N1        3
#define UART_LCR_STOP       (1 << 2)
#define UART_LCR_PARITY     (1 << 3)
#define UART_LCR_EPAR       (1 << 4)
#define UART_LCR_DLAB       (1 << 7)
#define UART_MCR_DTR        (1 << 0)
#define UART_MCR_RTS        (1 << 1)
#define UART_MCR_OUT2       (1 << 3)
#define UART_LSR_DR         (1 << 0)
#define UART_LSR_OE         (1 << 1)
#define UART_LSR_PE         (1 << 2)
#define UART_LSR_FE         (1 << 3)
#define UART_LSR_BI         (1 << 4)
#define UART_LSR_THRE       (1 << 5)
#define UART_LSR_TEMT       (1 << 6)
#define UART_LSR_ERRORS (UART_LSR_OE | UART_LSR_PE | UART_LSR_FE | UART_LSR_BI)

#define UART_BAUD       115200
#define UART_CLOCK_DIVISOR 16
#define UART_DIVISOR_MAX   0xffff
#define UART_RX_FIFO_SIZE  256
#define UART_TX_FIFO_SIZE  512
#define UART_TX_BATCH      16
#define UART_IRQ_BUDGET    256

struct ns16550a_port {
    volatile uint8_t *base;
    unsigned int irq;
    unsigned int clock;
    struct wait_queue rx_wait;
    struct wait_queue tx_wait;
    struct mutex tx_lock;
    struct kfifo rx_fifo;
    struct kfifo tx_fifo;
    uint8_t rx_buffer[UART_RX_FIFO_SIZE];
    uint8_t tx_buffer[UART_TX_FIFO_SIZE];
    uint64_t rx_overrun;
    uint64_t lsr_errors;
};

static struct ns16550a_port *ns16550a;

static uint8_t ns16550a_read_reg(struct ns16550a_port *port, unsigned int reg)
{
    return readb(port->base + reg);
}

static void ns16550a_write_reg(struct ns16550a_port *port, unsigned int reg,
                               uint8_t value)
{
    writeb(value, port->base + reg);
}

static uint8_t ns16550a_read_lsr(struct ns16550a_port *port)
{
    uint8_t lsr = ns16550a_read_reg(port, UART_LSR);

    if (lsr & UART_LSR_ERRORS)
        ++port->lsr_errors;
    return lsr;
}

static bool ns16550a_receive(struct ns16550a_port *port, unsigned int *budget)
{
    bool queued = false;

    while (*budget && (ns16550a_read_lsr(port) & UART_LSR_DR)) {
        uint8_t c = ns16550a_read_reg(port, UART_RX);

        --*budget;
        if (kfifo_put(&port->rx_fifo, c))
            queued = true;
        else
            ++port->rx_overrun;
    }
    return queued;
}

static bool ns16550a_transmit(struct ns16550a_port *port)
{
    unsigned int sent = 0;
    uint8_t c;

    while (sent < UART_TX_BATCH) {
        if (!(ns16550a_read_lsr(port) & UART_LSR_THRE))
            break;
        if (!kfifo_get(&port->tx_fifo, &c))
            break;
        ns16550a_write_reg(port, UART_TX, c);
        ++sent;
    }

    if (kfifo_is_empty(&port->tx_fifo))
        ns16550a_write_reg(port, UART_IER, UART_IER_RX);
    return sent;
}

static void ns16550a_irq(void *data)
{
    struct ns16550a_port *port = data;
    unsigned int budget = UART_IRQ_BUDGET;

    while (budget) {
        unsigned long flags;
        uint8_t iir = ns16550a_read_reg(port, UART_IIR);

        if (iir & UART_IIR_NO_INT)
            break;
        --budget;
        switch (iir & UART_IIR_ID) {
        case UART_IIR_RDI:
        case UART_IIR_RLSI:
        case UART_IIR_TIMEOUT: {
            bool queued;

            spin_lock_irqsave(&port->rx_wait.lock, flags);
            queued = ns16550a_receive(port, &budget);
            spin_unlock_irqrestore(&port->rx_wait.lock, flags);
            if (queued)
                wake_up_all(&port->rx_wait);
            break;
        }
        case UART_IIR_THRI: {
            bool progress;

            spin_lock_irqsave(&port->tx_wait.lock, flags);
            progress = ns16550a_transmit(port);
            spin_unlock_irqrestore(&port->tx_wait.lock, flags);
            if (progress)
                wake_up_all(&port->tx_wait);
            break;
        }
        default:
            Assert(false, "unexpected NS16550A interrupt %u\n",
                   (unsigned int)iir);
        }
    }
}

static void ns16550a_program(struct ns16550a_port *port, uint8_t lcr,
                             unsigned int divisor)
{
    ns16550a_write_reg(port, UART_LCR, lcr | UART_LCR_DLAB);
    ns16550a_write_reg(port, UART_DLL, divisor & 0xff);
    ns16550a_write_reg(port, UART_DLM, divisor >> 8);
    ns16550a_write_reg(port, UART_LCR, lcr);
}

static void ns16550a_hw_init(struct ns16550a_port *port)
{
    unsigned int divisor = port->clock / (UART_CLOCK_DIVISOR * UART_BAUD);

    ns16550a_write_reg(port, UART_IER, 0);
    ns16550a_program(port, UART_LCR_8N1, divisor);
    ns16550a_write_reg(port, UART_FCR,
                       UART_FCR_CONFIG | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
    ns16550a_write_reg(port, UART_MCR,
                       UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
    (void)ns16550a_read_lsr(port);
    (void)ns16550a_read_reg(port, UART_IIR);
    (void)ns16550a_read_reg(port, UART_MSR);
}

static uint8_t ns16550a_lcr(const struct termios2 *termios)
{
    uint8_t lcr = (termios->c_cflag & CSIZE) >> 4;

    if (termios->c_cflag & CSTOPB)
        lcr |= UART_LCR_STOP;
    if (termios->c_cflag & PARENB) {
        lcr |= UART_LCR_PARITY;
        if (!(termios->c_cflag & PARODD))
            lcr |= UART_LCR_EPAR;
    }
    return lcr;
}

int ns16550a_set_termios(const struct termios2 *termios)
{
    struct ns16550a_port *port = ns16550a;
    unsigned long flags;
    unsigned int prescale;
    unsigned int divisor;
    unsigned int actual;
    unsigned int baud = termios->c_ospeed;

    if (!port)
        return -ENODEV;
    if (!baud || baud > port->clock / UART_CLOCK_DIVISOR)
        return -EINVAL;
    prescale = UART_CLOCK_DIVISOR * baud;
    divisor = port->clock / prescale;
    if (port->clock % prescale >= prescale / 2)
        ++divisor;
    if (divisor > UART_DIVISOR_MAX)
        return -EINVAL;
    actual = port->clock / (UART_CLOCK_DIVISOR * divisor);
    if (actual < baud - baud / 50 || actual > baud + baud / 50)
        return -EINVAL;

    mutex_lock(&port->tx_lock);
    spin_lock_irqsave(&port->tx_wait.lock, flags);
    ns16550a_program(port, ns16550a_lcr(termios), divisor);
    spin_unlock_irqrestore(&port->tx_wait.lock, flags);
    mutex_unlock(&port->tx_lock);
    return 0;
}

static int ns16550a_probe(struct device *dev)
{
    const struct platform_device *pdev = to_platform_device(dev);
    const struct resource *mem = platform_get_resource(pdev, RES_MEM, 0);
    const struct resource *irq_res = platform_get_resource(pdev, RES_IRQ, 0);
    struct ns16550a_port *port;
    size_t offset;
    int ret;

    if (!mem || !irq_res)
        return -EINVAL;
    if (ns16550a || pdev->nres != 2 || mem->size <= UART_MSR)
        return -EOPNOTSUPP;
    offset = mem->start & (PAGE_SIZE - 1);
    if (mem->size > PAGE_SIZE - offset)
        return -EOPNOTSUPP;

    port = kzalloc(sizeof(*port));
    if (!port)
        return -ENOMEM;
    port->irq = irq_res->start;
    ret = platform_get_u32(pdev, "clock-frequency", &port->clock);
    if (ret)
        return ret;
    wait_queue_init(&port->rx_wait);
    wait_queue_init(&port->tx_wait);
    mutex_init(&port->tx_lock);
    Assert(
        !kfifo_init(&port->rx_fifo, port->rx_buffer, sizeof(port->rx_buffer)),
        "invalid NS16550A RX FIFO size\n");
    Assert(
        !kfifo_init(&port->tx_fifo, port->tx_buffer, sizeof(port->tx_buffer)),
        "invalid NS16550A TX FIFO size\n");
    ret = pgtable_map_io(UART_VBASE, mem->start - offset, PAGE_SIZE);
    if (ret)
        goto port;
    port->base = (volatile uint8_t *)(UART_VBASE + offset);
    ns16550a_hw_init(port);

    ret = irq_register(port->irq, ns16550a_irq, port);
    if (ret)
        goto port;
    ret = irq_enable(port->irq);
    if (ret)
        goto irq;
    ns16550a_write_reg(port, UART_IER, UART_IER_RX);

    dev->drvdata = port;
    ns16550a = port;
    return 0;

irq:
    Assert(!irq_unregister(port->irq), "NS16550A IRQ unregister failed\n");
port:
    kfree(port);
    return ret;
}

ssize_t ns16550a_read(void *buffer, size_t count)
{
    struct ns16550a_port *port = ns16550a;
    unsigned int len = MIN(count, (size_t)UART_RX_FIFO_SIZE);
    unsigned int received;
    int ret;

    if (!count)
        return 0;
    ret = wait_event_interruptible(
        &port->rx_wait, (received = kfifo_out(&port->rx_fifo, buffer, len)));
    if (ret)
        return -EINTR;
    return received;
}

ssize_t ns16550a_write(const void *buffer, size_t count)
{
    struct ns16550a_port *port = ns16550a;
    const uint8_t *bytes = buffer;
    size_t written = 0;
    int ret;

    if (!count)
        return 0;
    ret = mutex_lock_interruptible(&port->tx_lock);
    if (ret)
        return ret;
    while (written < count) {
        unsigned long flags;
        unsigned int chunk = MIN(count - written, (size_t)UART_TX_FIFO_SIZE);
        unsigned int queued;

        ret = wait_event_interruptible(&port->tx_wait,
                                       kfifo_avail(&port->tx_fifo));
        if (ret) {
            ret = -EINTR;
            break;
        }
        spin_lock_irqsave(&port->tx_wait.lock, flags);
        queued = kfifo_in(&port->tx_fifo, bytes + written, chunk);
        ns16550a_write_reg(port, UART_IER, UART_IER_RX | UART_IER_THRI);
        spin_unlock_irqrestore(&port->tx_wait.lock, flags);
        written += queued;
    }
    mutex_unlock(&port->tx_lock);
    return written ? (ssize_t)written : ret;
}

void ns16550a_flush_input(void)
{
    struct ns16550a_port *port = ns16550a;
    unsigned long flags;

    spin_lock_irqsave(&port->rx_wait.lock, flags);
    kfifo_reset(&port->rx_fifo);
    ns16550a_write_reg(port, UART_FCR, UART_FCR_CONFIG | UART_FCR_CLEAR_RX);
    spin_unlock_irqrestore(&port->rx_wait.lock, flags);
}

int ns16550a_flush_output(void)
{
    struct ns16550a_port *port = ns16550a;
    int ret;

    ret = wait_event_interruptible(&port->tx_wait,
                                   kfifo_is_empty(&port->tx_fifo));
    if (ret)
        return -EINTR;
    while (!(ns16550a_read_reg(port, UART_LSR) & UART_LSR_TEMT))
        barrier();
    return 0;
}

static const struct of_device_id ns16550a_ids[] = {
    {.compatible = "ns16550a"},
    {NULL},
};

static struct platform_driver ns16550a_driver = {
    .drv =
        {
            .name = "ns16550a",
            .bus = &platform_bus,
            .probe = ns16550a_probe,
        },
    .ids = ns16550a_ids,
};

int ns16550a_register(void)
{
    return driver_register(&ns16550a_driver.drv);
}

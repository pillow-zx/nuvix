#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/kfifo.h>
#include <nuvix/layout.h>
#include <nuvix/mutex.h>
#include <nuvix/ns16550a.h>
#include <nuvix/pgtable.h>
#include <nuvix/platform.h>
#include <nuvix/slab.h>
#include <nuvix/wait.h>
#include <asm/io.h>

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

struct ns16550a_port {
    volatile uint8_t *base;
    unsigned int irq;
    struct wait_queue rx_wait;
    struct mutex tx_lock;
    struct kfifo rx_fifo;
    uint8_t rx_buffer[UART_RX_FIFO_SIZE];
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

static void ns16550a_irq(void *data)
{
    struct ns16550a_port *port = data;
    unsigned long flags;
    unsigned int budget = UART_IRQ_BUDGET;
    bool queued = false;

    spin_lock_irqsave(&port->rx_wait.lock, flags);
    while (budget) {
        uint8_t iir = ns16550a_read_reg(port, UART_IIR);

        if (iir & UART_IIR_NO_INT)
            break;
        --budget;
        switch (iir & UART_IIR_ID) {
        case UART_IIR_RDI:
        case UART_IIR_RLSI:
        case UART_IIR_TIMEOUT:
            queued |= ns16550a_receive(port, &budget);
            break;
        default:
            Assert(false, "unexpected NS16550A interrupt %u\n",
                   (unsigned int)iir);
        }
    }
    spin_unlock_irqrestore(&port->rx_wait.lock, flags);
    if (queued)
        wake_up_all(&port->rx_wait);
}

static void ns16550a_hw_init(struct ns16550a_port *port)
{
    unsigned int divisor = UART_CLOCK / (UART_CLOCK_DIVISOR * UART_BAUD);

    ns16550a_write_reg(port, UART_LCR, UART_LCR_8N1);
    ns16550a_write_reg(port, UART_IER, 0);
    ns16550a_write_reg(port, UART_LCR, UART_LCR_8N1 | UART_LCR_DLAB);
    ns16550a_write_reg(port, UART_DLL, divisor & 0xff);
    ns16550a_write_reg(port, UART_DLM, divisor >> 8);
    ns16550a_write_reg(port, UART_LCR, UART_LCR_8N1);
    ns16550a_write_reg(port, UART_FCR,
                       UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
    ns16550a_write_reg(port, UART_MCR,
                       UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
    (void)ns16550a_read_lsr(port);
    (void)ns16550a_read_reg(port, UART_IIR);
    (void)ns16550a_read_reg(port, UART_MSR);
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
    wait_queue_init(&port->rx_wait);
    mutex_init(&port->tx_lock);
    Assert(
        !kfifo_init(&port->rx_fifo, port->rx_buffer, sizeof(port->rx_buffer)),
        "invalid NS16550A RX FIFO size\n");
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
    ns16550a_write_reg(port, UART_IER, UART_IER_RDI | UART_IER_RLSI);

    dev->drvdata = port;
    ns16550a = port;
    return 0;

irq:
    Assert(!irq_unregister(port->irq), "NS16550A IRQ unregister failed\n");
port:
    kfree(port);
    return ret;
}

size_t ns16550a_read(void *buffer, size_t count)
{
    struct ns16550a_port *port = ns16550a;
    unsigned int len = MIN(count, (size_t)UART_RX_FIFO_SIZE);
    unsigned int received;

    if (!count)
        return 0;
    wait_event(&port->rx_wait,
               (received = kfifo_out(&port->rx_fifo, buffer, len)));
    return received;
}

static bool ns16550a_transmit(struct ns16550a_port *port, uint8_t c)
{
    unsigned long flags;
    bool ready;

    spin_lock_irqsave(&port->rx_wait.lock, flags);
    ready = ns16550a_read_lsr(port) & UART_LSR_THRE;
    if (ready)
        ns16550a_write_reg(port, UART_TX, c);
    spin_unlock_irqrestore(&port->rx_wait.lock, flags);
    return ready;
}

size_t ns16550a_write(const void *buffer, size_t count)
{
    struct ns16550a_port *port = ns16550a;
    const uint8_t *bytes = buffer;

    if (!count)
        return 0;
    mutex_lock(&port->tx_lock);
    for (size_t i = 0; i < count; ++i)
        while (!ns16550a_transmit(port, bytes[i]))
            ;
    mutex_unlock(&port->tx_lock);
    return count;
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

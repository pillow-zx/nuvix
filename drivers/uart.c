/*
 * drivers/uart.c - NS16550A UART 驱动
 */

#include <drivers/uart.h>
#include <nuvix/tools.h>
#include <nuvix/dt.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/irq.h>
#include <nuvix/kfifo.h>
#include <nuvix/event.h>
#include <nuvix/spinlock.h>
#include <uapi/poll.h>
#include <nuvix/mmio.h>

struct uart_device {
	vaddr_t base;
	uint32_t reg_shift;
	uint32_t io_width;
	unsigned irq;
	unsigned fifo_depth;
	uint8_t ier;
	bool irq_started;
	bool rx_overflow_reported;
	atomic_t panic_mode;
	spinlock_t lock;
	unsigned char rx_storage[1024];
	unsigned char tx_storage[4096];
	struct kfifo rx;
	struct kfifo tx;
	struct wait_channel rx_wait;
	struct wait_channel tx_wait;
};

static struct uart_device uart = {
	.fifo_depth = 1,
	.lock = SPINLOCK_INIT,
	.rx = KFIFO_INIT(uart.rx_storage, 1, sizeof(uart.rx_storage)),
	.tx = KFIFO_INIT(uart.tx_storage, 1, sizeof(uart.tx_storage)),
	.rx_wait = WAIT_CHANNEL_INIT(uart.rx_wait),
	.tx_wait = WAIT_CHANNEL_INIT(uart.tx_wait),
};

static inline void uart_write_reg(struct uart_device *dev, int reg, uint8_t val)
{
	vaddr_t address = dev->base + ((unsigned)reg << dev->reg_shift);
	mmio_mb();
	if (dev->io_width == 4)
		MMIO_WRITE(uint32_t, address, val);
	else
		MMIO_WRITE(uint8_t, address, val);
	mmio_mb();
}

static inline uint8_t uart_read_reg(struct uart_device *dev, int reg)
{
	vaddr_t address = dev->base + ((unsigned)reg << dev->reg_shift);
	mmio_mb();
	uint8_t value = dev->io_width == 4 ? (uint8_t)MMIO_READ(uint32_t, address) :
		MMIO_READ(uint8_t, address);
	mmio_mb();
	return value;
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
	struct uart_device *dev = &uart;
	struct dt_resource resource;
	int irq = dt_irq(node, 0);
	if (irq < 0)
		panic("uart: invalid IRQ (%d)", irq);
	dev->irq = (unsigned)irq;
	dt_require_simple_device(node, false);
	if (dt_reg(node, 0, &resource))
		panic("uart: invalid reg");
	dev->reg_shift = uart_property(node, "reg-shift", 0);
	dev->io_width = uart_property(node, "reg-io-width", 1);
	uint32_t offset = uart_property(node, "reg-offset", 0);
	uint32_t clock = uart_property(node, "clock-frequency", 0);
	if (!baud)
		baud = uart_property(node, "current-speed", 115200);
	if (dev->reg_shift > 4 || (dev->io_width != 1 && dev->io_width != 4) ||
	    (dev->io_width == 4 &&
	     (dev->reg_shift < 2 || ((resource.start + offset) & 3))) ||
	    fdt_getprop(dt_blob, node, "big-endian", NULL) ||
	    offset > resource.size ||
	    ((7UL << dev->reg_shift) + dev->io_width) > resource.size - offset)
		panic("uart: unsupported register layout");
	if (!clock || !baud)
		panic("uart: clock-frequency and nonzero baud are required");
	uint64_t divisor = ((uint64_t)clock + (uint64_t)baud * 8) / ((uint64_t)baud * 16);
	if (!divisor || divisor > 65535)
		panic("uart: baud divisor out of range");
	dev->base = mmio_map(resource.start, resource.size) + offset;
	uart_write_reg(dev, UART_IER, 0x00);
	uart_write_reg(dev, UART_LCR, UART_LCR_DLAB);
	uart_write_reg(dev, 0, divisor & 0xff);
	uart_write_reg(dev, 1, divisor >> 8);
	uart_write_reg(dev, UART_LCR, UART_LCR_8N1);
	uart_write_reg(dev, UART_FCR, UART_FCR_EN | UART_FCR_CLR);
	dev->fifo_depth = (uart_read_reg(dev, UART_IIR) & 0xc0) == 0xc0 ? 16 : 1;
	uart_write_reg(dev, UART_MCR, 0x00);
}

/* These polling operations are reserved for boot and fatal diagnostics. */
void uart_poll_putchar(int ch)
{
	struct uart_device *dev = &uart;
	if (!dev->base)
		return;
	while (!(uart_read_reg(dev, UART_LSR) & UART_LSR_THRE))
		;
	uart_write_reg(dev, UART_THR, (uint8_t)ch);
}

void uart_panic_enter(void)
{
	struct uart_device *dev = &uart;
	atomic_set_release(&dev->panic_mode, 1);
	if (dev->base)
		uart_write_reg(dev, UART_IER, 0);
}

static void uart_update_ier(struct uart_device *dev)
{
	uart_write_reg(dev, UART_IER,
		       atomic_read_acquire(&dev->panic_mode) ? 0 : dev->ier);
}

/* Consume only available FIFO space: never wait for THRE to change. */
static bool uart_tx_kick(struct uart_device *dev)
{
	bool moved = false;
	if (uart_read_reg(dev, UART_LSR) & UART_LSR_THRE) {
		for (unsigned i = 0;
		     i < dev->fifo_depth && !kfifo_empty(&dev->tx); i++) {
			unsigned char ch;
			BUG_ON(kfifo_get(&dev->tx, &ch));
			uart_write_reg(dev, UART_THR, ch);
			moved = true;
		}
	}
	if (kfifo_empty(&dev->tx))
		dev->ier &= ~UART_IER_TX;
	else
		dev->ier |= UART_IER_TX;
	uart_update_ier(dev);
	return moved;
}

static void uart_handle_irq(unsigned irq, void *data)
{
	struct uart_device *dev = data;
	irq_flags_t flags;
	bool received = false, transmitted = false, overflow = false;
	(void)irq;

	spin_lock_irqsave(&dev->lock, flags);
	if (atomic_read_acquire(&dev->panic_mode)) {
		uart_write_reg(dev, UART_IER, 0);
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}
	for (;;) {
		uint8_t iir = uart_read_reg(dev, UART_IIR);
		if (iir & UART_IIR_NONE)
			break;
		switch (iir & UART_IIR_ID) {
		case UART_IIR_RX:
		case UART_IIR_TIMEOUT:
		case UART_IIR_LINE:
			for (;;) {
				uint8_t status = uart_read_reg(dev, UART_LSR);
				if (!(status & UART_LSR_DR))
					break;
				unsigned char ch = uart_read_reg(dev, UART_RBR);
				if ((status & UART_LSR_ERRORS) ||
				    kfifo_put(&dev->rx, &ch))
					overflow = true;
				else
					received = true;
			}
			break;
		case UART_IIR_TX:
			transmitted |= uart_tx_kick(dev);
			break;
		default:
			(void)uart_read_reg(dev, UART_MSR);
			break;
		}
	}
	overflow = overflow && !dev->rx_overflow_reported;
	dev->rx_overflow_reported |= overflow;
	spin_unlock_irqrestore(&dev->lock, flags);
	if (received)
		wait_channel_wake_all(&dev->rx_wait);
	if (transmitted)
		wait_channel_wake_all(&dev->tx_wait);
	if (overflow)
		pr_warn("uart: dropped input (receive error or full buffer)\n");
}

int uart_irq_start(void)
{
	struct uart_device *dev = &uart;
	irq_flags_t flags;
	int ret = irq_register(dev->irq, 0, uart_handle_irq, dev);
	if (ret)
		return ret;
	spin_lock_irqsave(&dev->lock, flags);
	dev->irq_started = true;
	dev->ier = UART_IER_RX | UART_IER_LINE;
	uart_write_reg(dev, UART_MCR, UART_MCR_OUT2);
	uart_update_ier(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
	return irq_enable(dev->irq);
}

int uart_try_getchar(void)
{
	struct uart_device *dev = &uart;
	irq_flags_t flags;
	unsigned char ch;
	spin_lock_irqsave(&dev->lock, flags);
	int ret = kfifo_get(&dev->rx, &ch);
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret ? -1 : ch;
}

int uart_rx_prepare(struct wait_scope *scope)
{
	struct uart_device *dev = &uart;
	irq_flags_t flags;
	spin_lock_irqsave(&dev->lock, flags);
	int ret = kfifo_empty(&dev->rx) ?
		wait_scope_prepare(scope, &dev->rx_wait, false) : 1;
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret;
}

ssize_t uart_try_write(const char *buf, size_t count, bool crlf)
{
	struct uart_device *dev = &uart;
	irq_flags_t flags;
	size_t consumed = 0;
	bool moved;
	if (!count)
		return 0;
	spin_lock_irqsave(&dev->lock, flags);
	if (!dev->irq_started || atomic_read_acquire(&dev->panic_mode)) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -EIO;
	}
	moved = uart_tx_kick(dev);
	while (consumed < count) {
		char ch = buf[consumed];
		bool expand = crlf && ch == '\n';
		if (kfifo_capacity(&dev->tx) - kfifo_size(&dev->tx) <
		    (expand ? 2u : 1u))
			break;
		if (expand) {
			char carriage_return = '\r';
			BUG_ON(kfifo_put(&dev->tx, &carriage_return));
		}
		BUG_ON(kfifo_put(&dev->tx, &ch));
		consumed++;
	}
	moved |= uart_tx_kick(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
	if (moved)
		wait_channel_wake_all(&dev->tx_wait);
	return consumed ? (ssize_t)consumed : -EAGAIN;
}

int uart_tx_poll(struct poll_table *wait, bool crlf)
{
	struct uart_device *dev = &uart;
	irq_flags_t flags;
	spin_lock_irqsave(&dev->lock, flags);
	int ret = poll_wait(wait, &dev->tx_wait);
	if (!ret) {
		if (!dev->irq_started || atomic_read_acquire(&dev->panic_mode))
			ret = POLLERR;
		else if (kfifo_capacity(&dev->tx) - kfifo_size(&dev->tx) >=
			 (crlf ? 2u : 1u))
			ret = POLLOUT;
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret;
}

int uart_write(const char *buf, size_t count, bool crlf)
{
	struct uart_device *dev = &uart;
	const struct wait_deadline deadline = wait_deadline_none();
	if (!wait_context_can_sleep())
		return -EINVAL;
	while (count) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		irq_flags_t flags;
		ssize_t written = uart_try_write(buf, count, crlf);
		if (written > 0) {
			buf += written;
			count -= written;
			continue;
		}
		if (written != -EAGAIN)
			return (int)written;
		int ret = wait_scope_begin(&scope, 0, &deadline);
		if (ret)
			return ret;
		spin_lock_irqsave(&dev->lock, flags);
		bool ready = kfifo_capacity(&dev->tx) - kfifo_size(&dev->tx) >=
			(crlf ? 2u : 1u);
		if (!ready)
			ret = wait_scope_prepare(&scope, &dev->tx_wait, false);
		spin_unlock_irqrestore(&dev->lock, flags);
		if (ret)
			return ret;
		if (!ready) {
			ret = wait_scope_block(&scope, &outcome);
			if (ret)
				return ret;
		}
	}
	return 0;
}

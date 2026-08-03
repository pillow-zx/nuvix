/*
 * kernel/printk.c - 内核日志与格式化输出
 */

#include <kernel/printk.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/stacktrace.h>
#include <kernel/processor.h>
#include <kernel/compiler.h>
#include <kernel/sbi.h>
#include <kernel/slab.h>
#include <kernel/spinlock.h>
#include <kernel/mutex.h>
#include <kernel/wait.h>
#include <drivers/uart.h>

constexpr size_t PRINTK_BUF_SIZE = 1024;
constexpr size_t PRINTK_LOG_BUF_SIZE = 4096;

struct printk_ring {
	spinlock_t lock;
	char storage[PRINTK_LOG_BUF_SIZE];
	uint64_t first_seq;
	uint64_t head_seq;
	uint64_t read_seq;
	uint64_t clear_seq;
	struct wait_channel read_wait;
	mutex_t read_lock;
};

static void (*console_putc)(int ch);
static bool printk_panic_mode;

static struct printk_ring printk_ring = {
	.lock = SPINLOCK_INIT,
	.read_wait = WAIT_CHANNEL_INIT(printk_ring.read_wait),
	.read_lock = MUTEX_INIT(printk_ring.read_lock),
};

static size_t printk_ring_normalize_locked(uint64_t *sequence)
{
	if (*sequence < printk_ring.first_seq)
		*sequence = printk_ring.first_seq;
	if (*sequence > printk_ring.head_seq)
		*sequence = printk_ring.head_seq;
	return (size_t)(printk_ring.head_seq - *sequence);
}

static void printk_ring_copy_locked(char *destination, uint64_t sequence,
				    size_t size)
{
	for (size_t index = 0; index < size; index++)
		destination[index] = printk_ring.storage[(sequence + index) %
							 PRINTK_LOG_BUF_SIZE];
}

static void printk_ring_append_locked(const char *source, size_t size)
{
	for (size_t index = 0; index < size; index++) {
		printk_ring
			.storage[printk_ring.head_seq % PRINTK_LOG_BUF_SIZE] =
			source[index];
		printk_ring.head_seq++;
		if (printk_ring.head_seq - printk_ring.first_seq >
		    PRINTK_LOG_BUF_SIZE)
			printk_ring.first_seq =
				printk_ring.head_seq - PRINTK_LOG_BUF_SIZE;
	}
	(void)printk_ring_normalize_locked(&printk_ring.read_seq);
	(void)printk_ring_normalize_locked(&printk_ring.clear_seq);
}

#ifdef KERNEL_SELFTEST
static void printk_ring_append(const char *source, size_t size)
{
	irq_flags_t flags;

	if (!source || size == 0)
		return;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	printk_ring_append_locked(source, size);
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	wait_channel_wake_one(&printk_ring.read_wait);
}
#endif

static uint32_t printk_log_level(int level)
{
	switch (level) {
	case LOG_ERROR:
		return 3;
	case LOG_WARNING:
		return 4;
	case LOG_NOTICE:
		return 5;
	case LOG_INFO:
		return 6;
	case LOG_DEBUG:
		return 7;
	default:
		return 6;
	}
}

static void printk_ring_append_message(int level, const char *message,
				       size_t size)
{
	const char priority[] = {
		'<',
		(char)('0' + printk_log_level(level)),
		'>',
	};
	irq_flags_t flags;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	printk_ring_append_locked(priority, sizeof(priority));
	printk_ring_append_locked(message, size);
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	wait_channel_wake_one(&printk_ring.read_wait);
}

static void console_write(const char *s)
{
	while (*s) {
		if (*s == '\n')
			console_putc('\r');
		console_putc(*s++);
	}
}

void console_init_sbi(void)
{
	console_putc = sbi_console_putchar;
}

void console_init_mmio(void)
{
	uart_init();
	console_putc = uart_putc;
}

size_t printk_log_buffer_size(void)
{
	return PRINTK_LOG_BUF_SIZE;
}

size_t printk_log_unread_size(void)
{
	irq_flags_t flags;
	size_t size;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	size = printk_ring_normalize_locked(&printk_ring.read_seq);
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	return size;
}

static int printk_log_read_probe(struct wait_session *session, void *arg)
{
	irq_flags_t flags;
	int ret;

	(void)arg;
	spin_lock_irqsave(&printk_ring.lock, &flags);
	if (printk_ring_normalize_locked(&printk_ring.read_seq) != 0) {
		spin_unlock_irqrestore(&printk_ring.lock, flags);
		return 1;
	}
	ret = wait_session_watch(session, &printk_ring.read_wait);
	if (ret < 0) {
		spin_unlock_irqrestore(&printk_ring.lock, flags);
		return ret;
	}
	ret = printk_ring_normalize_locked(&printk_ring.read_seq) != 0;
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	return ret;
}

static int printk_log_wait_for_unread(void)
{
	const struct wait_deadline deadline = wait_deadline_none();
	const struct wait_request source = {
		.kind = WAIT_KIND_GENERIC,
		.check = printk_log_read_probe,
		.channel_limit = 1,
	};
	wait_outcome_t outcome;
	int ret;

	ret = wait_for_interruptible(&source, &deadline, &outcome);
	if (ret < 0)
		return ret;
	if (outcome == WAIT_OUTCOME_SIGNAL)
		return -EINTR;
	BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	return 0;
}

ssize_t printk_log_read(void *buffer, size_t size)
{
	char *snapshot __cleanup_with(kfree) = nullptr;
	irq_flags_t flags;
	uint64_t sequence;
	size_t copied;
	int ret;

	if (!buffer)
		return -EINVAL;
	if (size == 0)
		return 0;
	if (size > PRINTK_LOG_BUF_SIZE)
		size = PRINTK_LOG_BUF_SIZE;
	snapshot = kmalloc(size);
	if (!snapshot)
		return -ENOMEM;

	mutex_lock(&printk_ring.read_lock);
	ret = printk_log_wait_for_unread();
	if (ret < 0)
		goto unlock;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	copied = printk_ring_normalize_locked(&printk_ring.read_seq);
	if (copied > size)
		copied = size;
	sequence = printk_ring.read_seq;
	printk_ring_copy_locked(snapshot, sequence, copied);
	spin_unlock_irqrestore(&printk_ring.lock, flags);

	if (copied != 0 && copy_to_user(buffer, snapshot, copied) != 0) {
		ret = -EFAULT;
		goto unlock;
	}

	spin_lock_irqsave(&printk_ring.lock, &flags);
	if (printk_ring.read_seq < sequence + copied)
		printk_ring.read_seq = sequence + copied;
	(void)printk_ring_normalize_locked(&printk_ring.read_seq);
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	ret = (int)copied;
unlock:
	mutex_unlock(&printk_ring.read_lock);
	return ret;
}

ssize_t printk_log_read_all(void *buffer, size_t size, bool clear)
{
	char *snapshot __cleanup_with(kfree) = nullptr;
	irq_flags_t flags;
	uint64_t sequence;
	uint64_t clear_to;
	size_t available;
	size_t copied;

	if (!buffer)
		return -EINVAL;
	if (size > PRINTK_LOG_BUF_SIZE)
		size = PRINTK_LOG_BUF_SIZE;
	if (size != 0) {
		snapshot = kmalloc(size);
		if (!snapshot)
			return -ENOMEM;
	}

	spin_lock_irqsave(&printk_ring.lock, &flags);
	sequence = printk_ring.clear_seq;
	available = printk_ring_normalize_locked(&sequence);
	clear_to = printk_ring.head_seq;
	copied = available;
	if (copied > size)
		copied = size;
	if (available > copied)
		sequence = printk_ring.head_seq - copied;
	if (copied != 0)
		printk_ring_copy_locked(snapshot, sequence, copied);
	spin_unlock_irqrestore(&printk_ring.lock, flags);

	if (copied != 0 && copy_to_user(buffer, snapshot, copied) != 0)
		return -EFAULT;

	if (!clear)
		return (ssize_t)copied;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	if (printk_ring.clear_seq < clear_to)
		printk_ring.clear_seq = clear_to;
	(void)printk_ring_normalize_locked(&printk_ring.clear_seq);
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	return (ssize_t)copied;
}

void printk_log_clear(void)
{
	irq_flags_t flags;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	printk_ring.clear_seq = printk_ring.head_seq;
	spin_unlock_irqrestore(&printk_ring.lock, flags);
}

static int vprintk(int level, const char *fmt, va_list ap)
{
	char message[PRINTK_BUF_SIZE];
	int formatted;
	size_t size;

	formatted = vsnprintf(message, sizeof(message), fmt, ap);
	if (formatted < 0)
		return formatted;
	size = (size_t)formatted;
	if (size >= sizeof(message))
		size = sizeof(message) - 1;
	if (size == 0)
		return formatted;
	if (printk_panic_mode) {
		if (console_putc)
			console_write(message);
		return formatted;
	}
	printk_ring_append_message(level, message, size);
	if (console_putc)
		console_write(message);
	return formatted;
}

int __printk(int level, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vprintk(level, fmt, ap);
	va_end(ap);
	return ret;
}

__noreturn
void __panic(const char *fmt, ...)
{
	/* Panic logging must remain usable even when the failure fills tracking. */
	printk_panic_mode = true;
	local_irq_disable();
	pr_err("\nKERNEL PANIC: ");

	va_list ap;
	va_start(ap, fmt);
	(void)vprintk(LOG_ERROR, fmt, ap);
	va_end(ap);
	pr_err("\n");

	pr_err("  sepc   = %p\n", (void *)(uintptr_t)trap_pc());
	pr_err("  scause = %p\n", (void *)(uintptr_t)trap_cause());
	pr_err("  stval  = %p\n", (void *)(uintptr_t)trap_value());
	pr_err("  ra     = %p\n", (void *)(uintptr_t)__return_address());
	pr_err("  sp     = %p\n", (void *)(uintptr_t)__frame_address());
	dump_stack();

	while (1)
		wait_for_interrupt();

	unreachable();
}

#ifdef KERNEL_SELFTEST
void printk_test_reset(void)
{
	irq_flags_t flags;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	memset(printk_ring.storage, 0, sizeof(printk_ring.storage));
	printk_ring.first_seq = 0;
	printk_ring.head_seq = 0;
	printk_ring.read_seq = 0;
	printk_ring.clear_seq = 0;
	spin_unlock_irqrestore(&printk_ring.lock, flags);
}

void printk_test_append(const char *data, size_t size)
{
	printk_ring_append(data, size);
}

size_t printk_test_read(char *buffer, size_t size)
{
	irq_flags_t flags;
	size_t copied;

	if (!buffer || size == 0)
		return 0;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	copied = printk_ring_normalize_locked(&printk_ring.read_seq);
	if (copied > size)
		copied = size;
	printk_ring_copy_locked(buffer, printk_ring.read_seq, copied);
	printk_ring.read_seq += copied;
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	return copied;
}

size_t printk_test_read_all(char *buffer, size_t size, bool clear)
{
	irq_flags_t flags;
	uint64_t sequence;
	uint64_t clear_to;
	size_t available;
	size_t copied;

	if (!buffer || size == 0)
		return 0;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	sequence = printk_ring.clear_seq;
	available = printk_ring_normalize_locked(&sequence);
	clear_to = printk_ring.head_seq;
	copied = available;
	if (copied > size)
		copied = size;
	if (available > copied)
		sequence = printk_ring.head_seq - copied;
	printk_ring_copy_locked(buffer, sequence, copied);
	if (clear && printk_ring.clear_seq < clear_to)
		printk_ring.clear_seq = clear_to;
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	return copied;
}
#endif

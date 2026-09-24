/*
 * kernel/printk.c - 内核日志与格式化输出
 */

#include <nuvix/printk.h>
#include <drivers/uart.h>
#include <nuvix/errno.h>
#include <nuvix/mm.h>
#include <nuvix/stacktrace.h>
#include <nuvix/processor.h>
#include <nuvix/compiler.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/spinlock.h>
#include <nuvix/mutex.h>
#include <nuvix/wait.h>

#define PRINTK_BUF_SIZE	    1024
#define PRINTK_LOG_BUF_SIZE 4096

struct printk_ring {
	spinlock_t lock;
	char storage[PRINTK_LOG_BUF_SIZE];
	/* Mark the syslog priority bytes so console output keeps its format. */
	uint8_t prefix[PRINTK_LOG_BUF_SIZE / 8];
	uint64_t console_seq;
	bool console_async;
	struct wait_channel console_wait;
	uint64_t first_seq;
	uint64_t head_seq;
	uint64_t read_seq;
	uint64_t clear_seq;
	struct wait_channel read_wait;
	mutex_t read_lock;
};

static atomic_t printk_panic_mode;

static void uart_poll_write(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_poll_putchar('\r');
		uart_poll_putchar(*s++);
	}
}

static struct printk_ring printk_ring = {
	.lock = SPINLOCK_INIT(LOCK_RANK_PRINTK_RING, LOCK_IRQ_HARDIRQ_REACHABLE),
	.read_wait = WAIT_CHANNEL_INIT_RANK(printk_ring.read_wait,
		LOCK_RANK_WAIT_CHANNEL, LOCK_IRQ_HARDIRQ_REACHABLE),
	.console_wait = WAIT_CHANNEL_INIT_RANK(printk_ring.console_wait,
		LOCK_RANK_WAIT_CHANNEL, LOCK_IRQ_HARDIRQ_REACHABLE),
	.read_lock = MUTEX_INIT(printk_ring.read_lock, LOCK_RANK_PRINTK_READ,
				LOCK_IRQ_TASK_ONLY),
};

__nonnull(1)
static inline size_t printk_ring_normalize_locked(uint64_t *sequence)
{
	if (*sequence < printk_ring.first_seq)
		*sequence = printk_ring.first_seq;
	if (*sequence > printk_ring.head_seq)
		*sequence = printk_ring.head_seq;
	return (size_t)(printk_ring.head_seq - *sequence);
}

__nonnull(1)
static void printk_ring_copy_locked(char *destination, uint64_t sequence, size_t size)
{
	for (size_t index = 0; index < size; index++)
		destination[index] = printk_ring.storage[(sequence + index) %
							 PRINTK_LOG_BUF_SIZE];
}

__nonnull(1)
static void printk_ring_append_locked(const char *source, size_t size, bool prefix)
{
	for (size_t index = 0; index < size; index++) {
		size_t slot = printk_ring.head_seq % PRINTK_LOG_BUF_SIZE;
		printk_ring.storage[slot] = source[index];
		if (prefix)
			printk_ring.prefix[slot / 8] |= 1u << (slot % 8);
		else
			printk_ring.prefix[slot / 8] &= ~(1u << (slot % 8));
		printk_ring.head_seq++;
		if (printk_ring.head_seq - printk_ring.first_seq >
		    PRINTK_LOG_BUF_SIZE)
			printk_ring.first_seq =
				printk_ring.head_seq - PRINTK_LOG_BUF_SIZE;
	}
	(void)printk_ring_normalize_locked(&printk_ring.read_seq);
	(void)printk_ring_normalize_locked(&printk_ring.clear_seq);
}

__must_check
static inline uint32_t printk_log_level(int level)
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

static void printk_ring_append_message(int level, const char *message, size_t size)
{
	const char priority[] = {
		'<',
		(char)('0' + printk_log_level(level)),
		'>',
	};
	irq_flags_t flags;

	spin_lock_irqsave(&printk_ring.lock, &flags);
	printk_ring_append_locked(priority, sizeof(priority), true);
	printk_ring_append_locked(message, size, false);
	bool async = printk_ring.console_async;
	if (!async) {
		/* Early boot only. The ring lock serializes the transition to
		 * asynchronous output with the last polled message. */
		uart_poll_write(message);
		printk_ring.console_seq = printk_ring.head_seq;
	}
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	wait_channel_wake_one(&printk_ring.read_wait);
	if (async)
		wait_channel_wake_one(&printk_ring.console_wait);
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

static int printk_log_wait_for_unread(void)
{
	const struct wait_deadline deadline = wait_deadline_none();

	for (;;) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool ready;

		ret = wait_scope_begin(&scope, WAIT_FLAG_INTERRUPTIBLE, &deadline);
		if (ret < 0)
			return ret;
		spin_lock_irqsave(&printk_ring.lock, &flags);
		ready = printk_ring_normalize_locked(&printk_ring.read_seq) !=
			0;
		if (!ready)
			ret = wait_scope_prepare(&scope, &printk_ring.read_wait,
						 true);
		spin_unlock_irqrestore(&printk_ring.lock, flags);
		if (ret < 0) {
			wait_scope_complete(&scope);
			return ret;
		}
		if (ready) {
			wait_scope_complete(&scope);
			return 0;
		}
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		if (ret < 0)
			return ret;
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
		BUG_ON(outcome == WAIT_OUTCOME_TIMEOUT);
	}
}

ssize_t printk_log_read(void *buffer, size_t size)
{
	char *snapshot __cleanup_with(kfree) = NULL;
	irq_flags_t flags;
	uint64_t sequence;
	size_t copied;
	size_t left = 0;
	int ret;

	if (!buffer)
		return -EINVAL;
	if (size == 0)
		return 0;
	if (size > PRINTK_LOG_BUF_SIZE)
		size = PRINTK_LOG_BUF_SIZE;
	snapshot = kmalloc(size, ALLOC_NOWAIT);
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

	if (copied != 0) {
		left = copy_to_user(buffer, snapshot, copied);
		copied -= left;
	}

	spin_lock_irqsave(&printk_ring.lock, &flags);
	if (printk_ring.read_seq < sequence + copied)
		printk_ring.read_seq = sequence + copied;
	(void)printk_ring_normalize_locked(&printk_ring.read_seq);
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	ret = copied ? (int)copied : (left ? -EFAULT : 0);
unlock:
	mutex_unlock(&printk_ring.read_lock);
	return ret;
}

ssize_t printk_log_read_all(void *buffer, size_t size, bool clear)
{
	char *snapshot __cleanup_with(kfree) = NULL;
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
		snapshot = kmalloc(size, ALLOC_NOWAIT);
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

static void printk_emit(int level, const char *message, size_t size)
{
	if (atomic_read_acquire(&printk_panic_mode)) {
		uart_poll_write(message);
		return;
	}
	printk_ring_append_message(level, message, size);
}

static void printk_console_thread(void *arg)
{
	const struct wait_deadline deadline = wait_deadline_none();
	(void)arg;

	for (;;) {
		char buffer[256];
		size_t count = 0;
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret = wait_scope_begin(&scope, 0, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&printk_ring.lock, &flags);
		/* Console backlog has the same bounded overwrite policy as the
		 * log ring; its cursor is independent of syslog readers/clear. */
		(void)printk_ring_normalize_locked(&printk_ring.console_seq);
		while (printk_ring.console_seq < printk_ring.head_seq &&
		       count < sizeof(buffer)) {
			size_t slot = printk_ring.console_seq++ % PRINTK_LOG_BUF_SIZE;
			if (!(printk_ring.prefix[slot / 8] & (1u << (slot % 8))))
				buffer[count++] = printk_ring.storage[slot];
		}
		if (!count)
			ret = wait_scope_prepare(&scope, &printk_ring.console_wait, true);
		spin_unlock_irqrestore(&printk_ring.lock, flags);
		BUG_ON(ret < 0);
		if (!count) {
			ret = wait_scope_block(&scope, &outcome);
			BUG_ON(ret < 0 || outcome != WAIT_OUTCOME_EVENT);
		} else {
			wait_scope_complete(&scope);
			if (uart_write(buffer, count, true) < 0)
				return;
		}
	}
}

int printk_console_start(void)
{
	irq_flags_t flags;
	spin_lock_irqsave(&printk_ring.lock, &flags);
	BUG_ON(printk_ring.console_async);
	printk_ring.console_async = true;
	spin_unlock_irqrestore(&printk_ring.lock, flags);
	return kernel_thread(printk_console_thread, NULL) ? 0 : -ENOMEM;
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
	printk_emit(level, message, size);
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

void __panic(const char *fmt, ...)
{
	/* Panic logging must remain usable even when the failure fills
	 * tracking. */
	atomic_set_release(&printk_panic_mode, 1);
	local_irq_disable();
	uart_panic_enter();
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

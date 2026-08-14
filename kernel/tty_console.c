/*
 * kernel/tty_console.c - UART-backed single-console TTY
 */

#include <drivers/uart.h>
#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/mm.h>
#include <nuvix/session.h>
#include <nuvix/task.h>
#include <nuvix/time.h>
#include <nuvix/timer.h>
#include <nuvix/tty.h>
#include <nuvix/vfs.h>
#include <nuvix/wait.h>
#include <uapi/signal.h>
#include <uapi/tty.h>

#include "tty_internal.h"

#define CONSOLE_INPUT_SIZE	256
#define CONSOLE_ECHO_SIZE	(CONSOLE_INPUT_SIZE * 3)

#define TTY_INPUT_CONTINUE	0
#define TTY_INPUT_READY		1
#define TTY_INPUT_EOF		2
#define TTY_INPUT_SIGNAL	3

typedef void (*console_emit_fn)(char ch, void *ctx);

struct console_input_state {
	spinlock_t lock;
	struct wait_channel readable;
	struct termios termios;
	struct winsize winsize;
	char data[CONSOLE_INPUT_SIZE];
	size_t len;
	size_t pos;
	bool eof;
	bool record_ready;
};

struct console_emit_buffer {
	char *data;
	size_t len;
	size_t cap;
};

struct console_read_wait {
	size_t count;
};

static ssize_t console_read(struct file *file, char *buf, size_t count);
static ssize_t console_write(struct file *file, const char *buf, size_t count);
static int console_poll(struct file *file, uint32_t events,
			struct task_wait *wait);
static int console_ioctl(struct file *file, uint64_t cmd, uint64_t arg);
static void console_input_thread(void *arg);

static struct console_input_state console_input = {
	.termios =
		{
			.c_iflag = BRKINT | ICRNL | IXON,
			.c_oflag = OPOST | ONLCR,
			.c_cflag = B38400 | CS8 | CREAD,
			.c_lflag = ISIG | ICANON | ECHO,
			.c_cc =
				{
					[VINTR] = 3,
					[VQUIT] = 28,
					[VERASE] = 127,
					[VKILL] = 21,
					[VEOF] = 4,
					[VTIME] = 0,
					[VMIN] = 1,
					[VSTART] = 17,
					[VSTOP] = 19,
					[VSUSP] = 26,
					[VEOL] = 0,
					[VREPRINT] = 18,
					[VDISCARD] = 15,
					[VWERASE] = 23,
					[VLNEXT] = 22,
					[VEOL2] = 0,
				},
		},
	.winsize =
		{
			.ws_row = 24,
			.ws_col = 80,
		},
};
static bool console_input_started;

static const struct file_operations console_fops = {
	.read = console_read,
	.write = console_write,
	.poll = console_poll,
	.ioctl = console_ioctl,
};

void tty_console_init(void)
{
	int ret;

	/* Ranks with the TTY: console lock regions may prepare on the
	 * readable wait channel (rank 30) while holding this lock. */
	spin_lock_init(&console_input.lock, LOCK_RANK_TTY);
	wait_channel_init(&console_input.readable);
	tty_console_endpoint_init();
	ret = vfs_register_chrdev(MKDEV(5, 1), &console_fops);
	BUG_ON(ret < 0);
}

int tty_console_start(void)
{
	struct task_struct *task;

	if (console_input_started)
		return 0;
	task = kernel_thread(console_input_thread, NULL);
	if (!task)
		return -ENOMEM;
	console_input_started = true;
	return 0;
}

static void console_uart_emit(char ch, void *ctx)
{
	(void)ctx;
	uart_putc(ch);
}

static void console_emit_output(const struct termios *termios, char ch,
				console_emit_fn emit, void *ctx)
{
	if ((termios->c_oflag & OPOST) && (termios->c_oflag & ONLCR) &&
	    ch == '\n')
		emit('\r', ctx);
	emit(ch, ctx);
}

static size_t console_write_translated(const struct termios *termios,
				       const char *buf, size_t count,
				       console_emit_fn emit, void *ctx)
{
	size_t emitted = 0;

	for (size_t i = 0; i < count; i++) {
		if ((termios->c_oflag & OPOST) && (termios->c_oflag & ONLCR) &&
		    buf[i] == '\n') {
			emit('\r', ctx);
			emitted++;
		}
		emit(buf[i], ctx);
		emitted++;
	}

	return emitted;
}

static void console_echo_char(const struct termios *termios, char ch,
			      console_emit_fn emit, void *ctx)
{
	if (!(termios->c_lflag & ECHO))
		return;
	console_emit_output(termios, ch, emit, ctx);
}

static void console_echo_erase(console_emit_fn emit, void *ctx)
{
	emit('\b', ctx);
	emit(' ', ctx);
	emit('\b', ctx);
}

static bool console_cc_eq(const struct termios *termios, int idx, char ch)
{
	return termios->c_cc[idx] != 0 && ch == (char)termios->c_cc[idx];
}

static int console_signal_for_char(const struct termios *termios, char ch)
{
	if (!(termios->c_lflag & ISIG))
		return 0;
	if (console_cc_eq(termios, VINTR, ch))
		return SIGINT;
	if (console_cc_eq(termios, VQUIT, ch))
		return SIGQUIT;
	if (console_cc_eq(termios, VSUSP, ch))
		return SIGTSTP;
	return 0;
}

static char console_translate_input(const struct termios *termios, char ch)
{
	if ((termios->c_iflag & ICRNL) && ch == '\r')
		return '\n';
	return ch;
}

static int console_canonical_accept(const struct termios *termios, char ch,
				    char *line, size_t *line_len,
				    size_t line_cap, console_emit_fn emit,
				    void *ctx, int *signal)
{
	int sig = console_signal_for_char(termios, ch);

	if (sig) {
		*line_len = 0;
		*signal = sig;
		return TTY_INPUT_SIGNAL;
	}

	ch = console_translate_input(termios, ch);

	if (console_cc_eq(termios, VEOF, ch))
		return *line_len == 0 ? TTY_INPUT_EOF : TTY_INPUT_READY;

	if (console_cc_eq(termios, VERASE, ch) || ch == '\b' || ch == 0x7f) {
		if (*line_len > 0) {
			(*line_len)--;
			if (termios->c_lflag & ECHO)
				console_echo_erase(emit, ctx);
		}
		return TTY_INPUT_CONTINUE;
	}

	if (console_cc_eq(termios, VKILL, ch)) {
		while (*line_len > 0) {
			(*line_len)--;
			if (termios->c_lflag & ECHO)
				console_echo_erase(emit, ctx);
		}
		return TTY_INPUT_CONTINUE;
	}

	if (*line_len + 1 < line_cap) {
		line[*line_len] = ch;
		(*line_len)++;
		console_echo_char(termios, ch, emit, ctx);
	}

	if (ch == '\n' || console_cc_eq(termios, VEOL, ch) ||
	    console_cc_eq(termios, VEOL2, ch))
		return TTY_INPUT_READY;
	return TTY_INPUT_CONTINUE;
}

static int console_raw_accept(const struct termios *termios, char raw,
			      char *out, size_t *out_len, size_t out_cap,
			      console_emit_fn emit, void *ctx, int *signal)
{
	int sig = console_signal_for_char(termios, raw);
	char ch;

	if (sig) {
		*signal = sig;
		return TTY_INPUT_SIGNAL;
	}

	ch = console_translate_input(termios, raw);
	if (*out_len < out_cap) {
		out[*out_len] = ch;
		(*out_len)++;
		console_echo_char(termios, ch, emit, ctx);
	}

	return TTY_INPUT_CONTINUE;
}

static void console_buffer_emit(char ch, void *ctx)
{
	struct console_emit_buffer *buf = ctx;

	if (buf->len < buf->cap)
		buf->data[buf->len] = ch;
	buf->len++;
}

static size_t console_input_available_locked(void)
{
	return console_input.len - console_input.pos;
}

static void console_input_compact_locked(void)
{
	size_t available = console_input_available_locked();

	if (console_input.pos == 0)
		return;
	memmove(console_input.data, console_input.data + console_input.pos,
		available);
	console_input.pos = 0;
	console_input.len = available;
}

static size_t console_raw_vmin_locked(size_t count)
{
	size_t vmin = console_input.termios.c_cc[VMIN];

	if (vmin == 0)
		vmin = 1;
	if (vmin > count)
		vmin = count;
	return vmin;
}

static bool console_input_readable_locked(size_t count)
{
	size_t available = console_input_available_locked();

	if (console_input.eof)
		return true;
	if (console_input.termios.c_lflag & ICANON)
		return console_input.record_ready && available > 0;
	return available >= console_raw_vmin_locked(count);
}

static bool console_input_pollable_locked(void)
{
	if (console_input.eof)
		return true;
	if (console_input.termios.c_lflag & ICANON)
		return console_input.record_ready &&
		       console_input_available_locked() > 0;
	return console_input_available_locked() > 0;
}

static ssize_t console_copy_pending_locked(char *buf, size_t count)
{
	size_t available = console_input_available_locked();
	size_t n = available < count ? available : count;

	memcpy(buf, console_input.data + console_input.pos, n);
	console_input.pos += n;
	if (console_input.pos == console_input.len) {
		console_input.pos = 0;
		console_input.len = 0;
		console_input.record_ready = false;
	}

	return (ssize_t)n;
}

static bool console_input_accept(char raw)
{
	char echo[CONSOLE_ECHO_SIZE];
	struct console_emit_buffer echo_buf = {
		.data = echo,
		.cap = sizeof(echo),
	};
	struct termios termios;
	irq_flags_t flags;
	int event;
	int signal = 0;
	bool wake = false;
	bool stop = false;

	spin_lock_irqsave(&console_input.lock, &flags);
	termios = console_input.termios;
	console_input_compact_locked();
	if (termios.c_lflag & ICANON) {
		event = console_canonical_accept(
			&termios, raw, console_input.data, &console_input.len,
			sizeof(console_input.data), console_buffer_emit,
			&echo_buf, &signal);
		if (event == TTY_INPUT_READY) {
			console_input.record_ready = true;
			wake = true;
			stop = true;
		} else if (event == TTY_INPUT_EOF) {
			console_input.eof = true;
			wake = true;
			stop = true;
		}
	} else {
		size_t before = console_input.len;

		event = console_raw_accept(
			&termios, raw, console_input.data, &console_input.len,
			sizeof(console_input.data), console_buffer_emit,
			&echo_buf, &signal);
		wake = console_input.len != before;
	}
	if (event == TTY_INPUT_SIGNAL) {
		console_input.pos = 0;
		console_input.len = 0;
		console_input.eof = false;
		console_input.record_ready = false;
		stop = true;
	}
	spin_unlock_irqrestore(&console_input.lock, flags);

	for (size_t i = 0; i < echo_buf.len && i < echo_buf.cap; i++)
		console_uart_emit(echo_buf.data[i], NULL);
	if (signal)
		(void)session_console_deliver_foreground_signal(signal);
	if (wake)
		wait_channel_wake_all(&console_input.readable);
	return stop;
}

static bool console_input_blocks_pump(void)
{
	irq_flags_t flags;
	bool blocked;

	spin_lock_irqsave(&console_input.lock, &flags);
	if (!(console_input.termios.c_lflag & ICANON))
		console_input_compact_locked();
	blocked = (console_input.termios.c_lflag & ICANON) &&
		  (console_input.record_ready || console_input.eof);
	if (!(console_input.termios.c_lflag & ICANON) &&
	    console_input.len == sizeof(console_input.data))
		blocked = true;
	spin_unlock_irqrestore(&console_input.lock, flags);
	return blocked;
}

static void console_input_drain_uart(void)
{
	while (!console_input_blocks_pump()) {
		int input = uart_try_getc();

		if (input < 0)
			return;
		if (console_input_accept((char)input))
			return;
	}
}

static void console_input_thread(void *arg)
{
	(void)arg;

	for (;;) {
		const struct wait_deadline deadline =
			wait_deadline_at(mtime_deadline_after(timer_now(),
							      CLOCKS_PER_TICK));
		int ret;

		console_input_drain_uart();
		ret = wait_sleep_until(&deadline);
		BUG_ON(ret < 0);
	}
}

static ssize_t console_read(struct file *file, char *buf, size_t count)
{
	const struct wait_deadline deadline = wait_deadline_none();
	struct task_wait *wait = &current_task()->wait;
	irq_flags_t flags;

	(void)file;
	if (count == 0)
		return 0;

	for (;;) {
		wait_outcome_t outcome;
		bool ready;
		int ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, &deadline);

		if (ret < 0)
			return ret;
		spin_lock_irqsave(&console_input.lock, &flags);
		ready = console_input_readable_locked(count);
		if (!ready)
			ret = wait_prepare(wait, &console_input.readable, true);
		spin_unlock_irqrestore(&console_input.lock, flags);
		if (ret < 0) {
			wait_finish(wait);
			return ret;
		}
		if (ready) {
			wait_finish(wait);
			continue;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		if (ret < 0)
			return ret;
		spin_lock_irqsave(&console_input.lock, &flags);
		if (outcome == WAIT_OUTCOME_SIGNAL) {
			if (!(console_input.termios.c_lflag & ICANON) &&
			    console_input_available_locked() > 0)
				ret = console_copy_pending_locked(buf, count);
			else
				ret = -EINTR;
			spin_unlock_irqrestore(&console_input.lock, flags);
			return ret;
		}
		if (console_input_available_locked() > 0 &&
		    console_input_readable_locked(count)) {
			ret = console_copy_pending_locked(buf, count);
			spin_unlock_irqrestore(&console_input.lock, flags);
			return ret;
		}
		if (console_input.eof) {
			console_input.eof = false;
			spin_unlock_irqrestore(&console_input.lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&console_input.lock, flags);
	}
}

static ssize_t console_write(struct file *file, const char *buf, size_t count)
{
	struct termios termios;
	irq_flags_t flags;

	(void)file;

	spin_lock_irqsave(&console_input.lock, &flags);
	termios = console_input.termios;
	spin_unlock_irqrestore(&console_input.lock, flags);
	console_write_translated(&termios, buf, count, console_uart_emit, NULL);

	return (ssize_t)count;
}

static int console_poll(struct file *file, uint32_t events,
			struct task_wait *wait)
{
	irq_flags_t flags;
	uint32_t mask = 0;
	int ret;

	if ((events & POLLIN) && (file->f_mode & FMODE_READ)) {
		spin_lock_irqsave(&console_input.lock, &flags);
		if (wait) {
			ret = wait_prepare(wait, &console_input.readable, false);
			if (ret < 0) {
				spin_unlock_irqrestore(&console_input.lock,
						       flags);
				return ret;
			}
		}
		if (console_input_pollable_locked())
			mask |= POLLIN;
		spin_unlock_irqrestore(&console_input.lock, flags);
	}
	if ((events & POLLOUT) && (file->f_mode & FMODE_WRITE))
		mask |= POLLOUT;
	return mask;
}

static int console_ioctl(struct file *file, uint64_t cmd, uint64_t arg)
{
	struct termios termios;
	struct winsize winsize;
	irq_flags_t flags;
	pid_t pid;
	int ret;

	(void)file;

	switch (cmd) {
	case TCGETS:
		spin_lock_irqsave(&console_input.lock, &flags);
		termios = console_input.termios;
		spin_unlock_irqrestore(&console_input.lock, flags);
		if (copy_to_user((void *)arg, &termios, sizeof(termios)) != 0)
			return -EFAULT;
		return 0;
	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		if (copy_from_user(&termios, (const void *)arg,
				   sizeof(termios)) != 0)
			return -EFAULT;
		spin_lock_irqsave(&console_input.lock, &flags);
		console_input.termios = termios;
		spin_unlock_irqrestore(&console_input.lock, flags);
		wait_channel_wake_all(&console_input.readable);
		return 0;
	case TIOCSCTTY:
		return session_console_acquire((int)arg);
	case TIOCNOTTY:
		return session_console_release();
	case TIOCGPGRP:
		ret = session_console_get_foreground_pgid(&pid);
		if (ret < 0)
			return ret;
		if (copy_to_user((void *)arg, &pid, sizeof(pid)) != 0)
			return -EFAULT;
		return 0;
	case TIOCSPGRP:
		if (copy_from_user(&pid, (const void *)arg, sizeof(pid)) != 0)
			return -EFAULT;
		return session_console_set_foreground_pgid(pid);
	case TIOCGWINSZ:
		spin_lock_irqsave(&console_input.lock, &flags);
		winsize = console_input.winsize;
		spin_unlock_irqrestore(&console_input.lock, flags);
		if (copy_to_user((void *)arg, &winsize, sizeof(winsize)) != 0)
			return -EFAULT;
		return 0;
	case TIOCSWINSZ:
		if (copy_from_user(&winsize, (const void *)arg,
				   sizeof(winsize)) != 0)
			return -EFAULT;
		spin_lock_irqsave(&console_input.lock, &flags);
		console_input.winsize = winsize;
		spin_unlock_irqrestore(&console_input.lock, flags);
		return 0;
	case TIOCGSID:
		ret = session_console_get_sid(&pid);
		if (ret < 0)
			return ret;
		if (copy_to_user((void *)arg, &pid, sizeof(pid)) != 0)
			return -EFAULT;
		return 0;
	}

	return -ENOTTY;
}

/*
 * fs/pipe.c - 管道
 */

#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/pipe.h>
#include <nuvix/slab.h>
#include <nuvix/signal.h>
#include <nuvix/spinlock.h>
#include <nuvix/task.h>
#include <nuvix/wait.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>

#define PIPE_SIZE PAGE_SIZE

struct pipe_buffer {
	spinlock_t lock;
	refcount_t refs;
	uint8_t *data;
	size_t head;
	size_t tail;
	size_t used;

	int readers;
	int writers;
	bool consume_active;
	uint64_t consume_generation;
	size_t consume_length;
	struct wait_channel readers_wq;
	struct wait_channel writers_wq;
};

struct pipe_write_wait {
	struct pipe_buffer *pipe;
	size_t min_space;
};

static void pipe_put(struct pipe_buffer *pipe);
CLEANUP_DEFINE(pipe_ref, struct pipe_buffer *, if (_T) pipe_put(_T));

static void pipe_get(struct pipe_buffer *pipe)
{
	if (pipe)
		refcount_inc(&pipe->refs);
}

static ssize_t pipe_read(struct file *file, char *buf, size_t count, loff_t pos);
static ssize_t pipe_write(struct file *file, const char *buf, size_t count,
			  loff_t pos);
static int pipe_poll(struct file *file, uint32_t events,
		     struct task_wait *wait);
static int pipe_release(struct file *file);

static const struct file_operations pipe_read_fops = {
	.read = pipe_read,
	.poll = pipe_poll,
	.release = pipe_release,
};

static const struct file_operations pipe_write_fops = {
	.write = pipe_write,
	.poll = pipe_poll,
	.release = pipe_release,
};

bool pipe_file(struct file *file)
{
	return file && (file->f_op == &pipe_read_fops ||
			file->f_op == &pipe_write_fops);
}

static struct file *pipe_file_alloc(const struct file_operations *f_op,
				    uint32_t mode, void *private_data)
{
	return file_alloc(f_op, mode, private_data);
}

static size_t pipe_linear_tail(struct pipe_buffer *pipe)
{
	size_t until_end = PIPE_SIZE - pipe->tail;

	if (pipe->used < until_end)
		return pipe->used;
	return until_end;
}

static size_t pipe_linear_head_space(struct pipe_buffer *pipe)
{
	size_t space = PIPE_SIZE - pipe->used;
	size_t until_end = PIPE_SIZE - pipe->head;

	if (space < until_end)
		return space;
	return until_end;
}

static int pipe_wait(struct pipe_buffer *pipe, bool writing,
			 size_t min_space)
{
	const struct wait_deadline deadline = wait_deadline_none();
	struct task_wait *wait = &current_task()->wait;

	for (;;) {
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool ready;

		ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, &deadline);
		if (ret < 0)
			return ret;
		spin_lock_irqsave(&pipe->lock, &flags);
		ready = writing ? PIPE_SIZE - pipe->used >= min_space ||
					pipe->readers == 0
				: (!pipe->consume_active &&
				   (pipe->used > 0 || pipe->writers == 0));
		if (!ready)
			ret = wait_prepare(wait,
					writing ? &pipe->writers_wq : &pipe->readers_wq,
					true);
		spin_unlock_irqrestore(&pipe->lock, flags);
		if (ret < 0) {
			wait_finish(wait);
			return ret;
		}
		if (ready) {
			wait_finish(wait);
			return 0;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		if (ret < 0)
			return ret;
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
		BUG_ON(outcome == WAIT_OUTCOME_TIMEOUT);
	}
}

static void pipe_commit_read_locked(struct pipe_buffer *pipe, size_t count)
{
	pipe->tail = (pipe->tail + count) % PIPE_SIZE;
	pipe->used -= count;
}

static int pipe_consume_wait(struct pipe_buffer *pipe, struct file *file)
{
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	return pipe_wait(pipe, false, 0);
}

static int pipe_consume_finish(struct pipe_consume_token *token, size_t count,
			       bool commit)
{
	struct pipe_buffer *pipe;
	irq_flags_t flags;

	if (!token || !token->active || !token->pipe ||
	    count > token->length)
		return -EINVAL;
	pipe = token->pipe;
	spin_lock_irqsave(&pipe->lock, &flags);
	if (!pipe->consume_active || pipe->consume_generation != token->generation ||
	    pipe->consume_length != token->length) {
		spin_unlock_irqrestore(&pipe->lock, flags);
		return -EINVAL;
	}
	if (commit && count)
		pipe_commit_read_locked(pipe, count);
	pipe->consume_active = false;
	pipe->consume_length = 0;
	spin_unlock_irqrestore(&pipe->lock, flags);
	token->active = false;
	token->pipe = NULL;
	wait_channel_wake_all(&pipe->writers_wq);
	wait_channel_wake_all(&pipe->readers_wq);
	pipe_put(pipe);
	return 0;
}

static void pipe_commit_write_locked(struct pipe_buffer *pipe, const char *buf,
				     size_t count)
{
	size_t done = 0;

	while (done < count) {
		size_t chunk = count - done;
		size_t linear = pipe_linear_head_space(pipe);

		if (chunk > linear)
			chunk = linear;
		if (!virt_to_page(pipe->data))
			pr_err("pipe: invalid data pipe=%p data=%p head=%zu used=%zu "
			       "buf=%p count=%zu pid=%u\n",
			       pipe, pipe->data, pipe->head, pipe->used, buf, count,
			       current_task()->tid ? current_task()->tid->nr : 0);
		memcpy(pipe->data + pipe->head, buf + done, chunk);
		pipe->head = (pipe->head + chunk) % PIPE_SIZE;
		pipe->used += chunk;
		done += chunk;
	}

}

static struct pipe_buffer *pipe_buffer_alloc(void)
{
	struct pipe_buffer *pipe = kmalloc(sizeof(*pipe), ALLOC_NOWAIT);
	if (!pipe)
		return NULL;

	memset(pipe, 0, sizeof(*pipe));
	spin_lock_init(&pipe->lock, LOCK_RANK_PIPE, LOCK_IRQ_TASK_ONLY);
	refcount_set(&pipe->refs, 1); /* construction reference */
	pipe->data = get_free_page(0, ALLOC_NOWAIT);
	if (!pipe->data) {
		kfree(pipe);
		return NULL;
	}

	wait_channel_init(&pipe->readers_wq);
	wait_channel_init(&pipe->writers_wq);

	return pipe;
}

static void pipe_buffer_free(struct pipe_buffer *pipe)
{
	if (!pipe)
		return;

	if (pipe->data)
		free_page(pipe->data, 0);
	kfree(pipe);
}

static void pipe_put(struct pipe_buffer *pipe)
{
	if (pipe && refcount_dec_and_test(&pipe->refs))
		pipe_buffer_free(pipe);
}

static ssize_t pipe_read(struct file *file, char *buf, size_t count, loff_t pos)
{
	struct pipe_buffer *pipe = file->private_data;
	struct pipe_buffer *__cleanup_with(pipe_ref) held = NULL;
	size_t done = 0;
	irq_flags_t flags;

	(void)pos;
	if (!pipe)
		return -EINVAL;
	pipe_get(pipe);
	held = pipe;

	while (done < count) {
		size_t chunk;
		size_t linear;

		spin_lock_irqsave(&pipe->lock, &flags);
		if (pipe->consume_active || pipe->used == 0) {
			if (pipe->consume_active) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				if (file->f_flags & O_NONBLOCK)
					return done ? (ssize_t)done : -EAGAIN;
				int wait_ret = pipe_wait(pipe, false, 0);
				if (wait_ret < 0)
					return done ? (ssize_t)done : wait_ret;
				continue;
			}
			if (pipe->writers == 0 || done > 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				break;
			}
			if (file->f_flags & O_NONBLOCK) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return -EAGAIN;
			}
			spin_unlock_irqrestore(&pipe->lock, flags);

			int ret = pipe_wait(pipe, false, 0);
			if (ret < 0)
				return done ? (ssize_t)done : ret;
			continue;
		}

		chunk = count - done;
		linear = pipe_linear_tail(pipe);

		if (chunk > linear)
			chunk = linear;

		memcpy(buf + done, pipe->data + pipe->tail, chunk);
		pipe_commit_read_locked(pipe, chunk);
		spin_unlock_irqrestore(&pipe->lock, flags);
		wait_channel_wake_all(&pipe->writers_wq);
		done += chunk;
	}

	return (ssize_t)done;
}

static ssize_t pipe_write(struct file *file, const char *buf, size_t count,
			  loff_t pos)
{
	struct pipe_buffer *pipe = file->private_data;
	struct pipe_buffer *__cleanup_with(pipe_ref) held = NULL;
	struct pipe_write_wait wait = {
		.pipe = pipe,
		.min_space = count <= PIPE_BUF ? count : 1,
	};
	bool atomic = count <= PIPE_BUF;
	size_t done = 0;
	irq_flags_t flags;

	(void)pos;
	if (!pipe)
		return -EINVAL;
	pipe_get(pipe);
	held = pipe;
	if (count == 0)
		return 0;

	while (done < count) {
		size_t space;

		spin_lock_irqsave(&pipe->lock, &flags);
		if (pipe->readers == 0) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			if (done == 0)
				(void)sig_send_self(SIGPIPE);
			return done ? (ssize_t)done : -EPIPE;
		}

		space = PIPE_SIZE - pipe->used;
		if (space < wait.min_space) {
			if (file->f_flags & O_NONBLOCK) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return done ? (ssize_t)done : -EAGAIN;
			}
			spin_unlock_irqrestore(&pipe->lock, flags);

			int ret = pipe_wait(pipe, true, wait.min_space);
			if (ret < 0)
				return done ? (ssize_t)done : ret;
			continue;
		}

		if (atomic) {
			pipe_commit_write_locked(pipe, buf, count);
			spin_unlock_irqrestore(&pipe->lock, flags);
			wait_channel_wake_all(&pipe->readers_wq);
			return (ssize_t)count;
		}

		if (space > count - done)
			space = count - done;
		pipe_commit_write_locked(pipe, buf + done, space);
		done += space;
		spin_unlock_irqrestore(&pipe->lock, flags);
		wait_channel_wake_all(&pipe->readers_wq);
	}

	return (ssize_t)done;
}

static int pipe_poll(struct file *file, uint32_t events,
			     struct task_wait *wait)
{
	struct pipe_buffer *pipe = file->private_data;
	struct pipe_buffer *__cleanup_with(pipe_ref) held = NULL;
	uint32_t mask = 0;
	irq_flags_t flags;
	int ret;

	if (!pipe)
		return POLLERR;
	pipe_get(pipe);
	held = pipe;
	spin_lock_irqsave(&pipe->lock, &flags);
	if ((events & POLLIN) && (file->f_mode & FMODE_READ)) {
		if (wait) {
			ret = wait_prepare(wait, &pipe->readers_wq, false);
			if (ret < 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return ret;
			}
		}
		if (pipe->used > 0)
			mask |= POLLIN;
		if (pipe->writers == 0)
			mask |= POLLHUP;
	}
	if ((events & POLLOUT) && (file->f_mode & FMODE_WRITE)) {
		if (wait) {
			ret = wait_prepare(wait, &pipe->writers_wq, false);
			if (ret < 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return ret;
			}
		}
		if (pipe->readers == 0)
			mask |= POLLERR;
		else if (pipe->used < PIPE_SIZE)
			mask |= POLLOUT;
	}
	spin_unlock_irqrestore(&pipe->lock, flags);
	return mask;
}

static int pipe_release(struct file *file)
{
	struct pipe_buffer *pipe = file->private_data;
	irq_flags_t flags;
	bool wake_writers = false;
	bool wake_readers = false;

	if (!pipe)
		return 0;
	/* Keep the wait channels alive while the endpoint wakeups run. */
	pipe_get(pipe);

	spin_lock_irqsave(&pipe->lock, &flags);
	if (file->f_mode & FMODE_READ) {
		if (pipe->readers > 0)
			pipe->readers--;
		if (pipe->readers == 0)
			wake_writers = true;
	}

	if (file->f_mode & FMODE_WRITE) {
		if (pipe->writers > 0)
			pipe->writers--;
		if (pipe->writers == 0)
			wake_readers = true;
	}

	spin_unlock_irqrestore(&pipe->lock, flags);
	if (wake_writers)
		wait_channel_wake_all(&pipe->writers_wq);
	if (wake_readers)
		wait_channel_wake_all(&pipe->readers_wq);

	pipe_put(pipe); /* endpoint reference */
	pipe_put(pipe); /* temporary release reference */

	return 0;
}

int pipe_consume_begin(struct file *file, char *buffer, size_t count,
		       struct pipe_consume_token *token)
{
	struct pipe_buffer *pipe = file ? file->private_data : NULL;
	struct pipe_buffer *__cleanup_with(pipe_ref) held = NULL;
	irq_flags_t flags;

	if (!pipe || !buffer || !token || count == 0)
		return -EINVAL;
	memset(token, 0, sizeof(*token));
	pipe_get(pipe);
	held = pipe;
	for (;;) {
		size_t chunk;
		if (!pipe->readers)
			return -EPIPE;
		spin_lock_irqsave(&pipe->lock, &flags);
		if (pipe->consume_active || pipe->used == 0) {
			bool eof = pipe->used == 0 && pipe->writers == 0;

			spin_unlock_irqrestore(&pipe->lock, flags);
			if (eof)
				return 0;
			int ret = pipe_consume_wait(pipe, file);
			if (ret < 0)
				return ret;
			continue;
		}
		chunk = count;
		if (chunk > pipe_linear_tail(pipe))
			chunk = pipe_linear_tail(pipe);
		memcpy(buffer, pipe->data + pipe->tail, chunk);
		pipe->consume_active = true;
		pipe->consume_generation++;
		pipe->consume_length = chunk;
		token->pipe = pipe;
		token->generation = pipe->consume_generation;
		token->length = chunk;
		token->active = true;
		spin_unlock_irqrestore(&pipe->lock, flags);
		cleanup_forget_ptr(held);
		return (int)chunk;
	}
}

int pipe_consume_commit(struct pipe_consume_token *token, size_t count)
{
	return pipe_consume_finish(token, count, true);
}

void pipe_consume_abort(struct pipe_consume_token *token)
{
	(void)pipe_consume_finish(token, 0, false);
}

ssize_t pipe_splice_to_file(struct file *pipe_file, struct file *out_file,
			    loff_t *out_offset, size_t len)
{
	struct pipe_buffer *pipe = pipe_file ? pipe_file->private_data : NULL;
	struct pipe_buffer *__cleanup_with(pipe_ref) held = NULL;
	char *buffer __cleanup_with(page0) = NULL;
	size_t done = 0;

	if (!pipe || !out_file)
		return -EINVAL;
	pipe_get(pipe);
	held = pipe;

	while (done < len) {
		size_t chunk = len - done;
		ssize_t ret;
		struct pipe_consume_token token;

		if (!buffer) {
			buffer = get_free_page(0, ALLOC_NOWAIT);
			if (!buffer)
				return done ? (ssize_t)done : -ENOMEM;
		}
		ret = pipe_consume_begin(pipe_file, buffer, chunk, &token);
		if (ret < 0)
			return done ? (ssize_t)done : ret;
		if (ret == 0)
			break;
		chunk = (size_t)ret;

		ret = vfs_write_pos(out_file, buffer, chunk, out_offset);

		if (ret < 0) {
			pipe_consume_abort(&token);
			return done ? (ssize_t)done : ret;
		}
		if (ret == 0) {
			pipe_consume_abort(&token);
			break;
		}
		if ((size_t)ret > chunk)
			ret = (ssize_t)chunk;
		if (pipe_consume_commit(&token, (size_t)ret) < 0)
			return done ? (ssize_t)done : -EIO;
		done += (size_t)ret;
		if ((size_t)ret < chunk)
			break;
	}

	return (ssize_t)done;
}

int do_pipe2(int fds[2], int flags)
{
	uint32_t status_flags = (uint32_t)flags & O_NONBLOCK;
	int fd_flags = flags & O_CLOEXEC;

	if (flags & ~(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;
	if (!fds)
		return -EINVAL;

	struct pipe_buffer *pipe = pipe_buffer_alloc();
	if (!pipe)
		return -ENOMEM;

	pipe->readers = 1;

	struct file *read_file =
		pipe_file_alloc(&pipe_read_fops, FMODE_READ, pipe);
	if (!read_file) {
		pipe_put(pipe);
		return -ENOMEM;
	}
	pipe_get(pipe); /* read endpoint */
	read_file->f_flags = status_flags | O_RDONLY;

	struct file *write_file =
		pipe_file_alloc(&pipe_write_fops, FMODE_WRITE, pipe);
	if (!write_file) {
		file_put(read_file);
		pipe_put(pipe); /* construction reference */
		return -ENOMEM;
	}
	pipe->writers = 1;
	pipe_get(pipe); /* write endpoint */
	write_file->f_flags = status_flags | O_WRONLY;
	pipe_put(pipe); /* drop construction reference */

	fds[0] = fd_alloc_flags(read_file, fd_flags);
	if (fds[0] < 0) {
		file_put(read_file);
		file_put(write_file);
		return fds[0];
	}

	fds[1] = fd_alloc_flags(write_file, fd_flags);
	if (fds[1] < 0) {
		fd_close(fds[0]);
		file_put(write_file);
		return fds[1];
	}

	return 0;
}

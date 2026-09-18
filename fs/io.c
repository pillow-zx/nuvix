#include <nuvix/io.h>
#include <nuvix/errno.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>

ssize_t io_wait_transfer(struct file *file, void *buffer, size_t length,
			 bool write)
{
	struct wait_deadline deadline = wait_deadline_none();
	struct wait_entry entries[2];

	if (!file->f_op || !file->f_op->try_io)
		return -EOPNOTSUPP;
	for (;;) {
		struct wait_scope scope = {.entries = entries, .capacity = 2};
		wait_outcome_t outcome;
		ssize_t ret = wait_scope_begin(&scope, WAIT_FLAG_INTERRUPTIBLE,
					       &deadline);
		if (ret < 0)
			return ret;
		ret = vfs_poll(file, write ? POLLOUT : POLLIN, scope.wait);
		if (ret >= 0)
			ret = file->f_op->try_io(file, buffer, length, write);
		if (ret != -EAGAIN || (file->f_flags & O_NONBLOCK)) {
			wait_scope_complete(&scope);
			if (ret == -EPIPE && write)
				(void)sig_send_self(SIGPIPE);
			return ret;
		}
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		if (ret < 0)
			return ret;
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
	}
}

#include <nuvix/errno.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>
#include <nuvix/io.h>
#include <nuvix/page_cache.h>
#include <nuvix/string.h>

static void io_flush_complete(struct blk_request *block, int result)
{
	struct io_request *req = block->private_data;
	irq_flags_t flags;
	spin_lock_irqsave(&req->flush_lock, flags);
	req->flush_result = result;
	req->flush_done = true;
	wait_channel_wake_all(&req->completion);
	spin_unlock_irqrestore(&req->flush_lock, flags);
}

bool io_request_flush_done(struct io_request *req)
{
	irq_flags_t flags;
	bool done;
	spin_lock_irqsave(&req->flush_lock, flags);
	done = req->flush_done;
	spin_unlock_irqrestore(&req->flush_lock, flags);
	return done;
}

void io_request_init(struct io_request *req, struct file *file,
		     enum io_request_op op, void *buffer, size_t length,
		     loff_t offset, bool datasync)
{
	memset(req, 0, sizeof(*req));
	req->file = file;
	req->op = op;
	req->buffer = buffer;
	req->length = length;
	req->offset = offset;
	req->datasync = datasync;
	wait_channel_init(&req->completion);
	spin_lock_init(&req->flush_lock);
}

ssize_t io_request_progress(struct io_request *req)
{
	struct file *file = req->file;
	ssize_t ret;
	if (!file || !file->f_op)
		return -EBADF;
	if (req->op == IO_REQUEST_FSYNC) {
		struct blkdev *bdev;
		if (!file->f_inode || !file->f_inode->i_sb ||
		    !file->f_op->try_fsync_prepare)
			return -EOPNOTSUPP;
		if (req->stage == 0) {
			ret = file->f_op->try_fsync_prepare(file, req->datasync);
			if (ret < 0)
				return ret;
			req->stage = 1;
		}
		if (req->stage == 1) {
			ret = pgcache_sync_device_step(file->f_inode->i_sb->s_dev);
			if (ret == -EAGAIN)
				req->irreversible = true;
			if (ret < 0)
				return ret;
			req->stage = 2;
		}
		if (req->stage == 2) {
			bdev = lookup_blkdev(file->f_inode->i_sb->s_dev);
			if (!bdev || !bdev->bd_ops || !bdev->bd_ops->submit)
				return -ENXIO;
			req->flush = (struct blk_request){
				.op = BLK_REQUEST_FLUSH,
				.complete = io_flush_complete,
				.private_data = req,
			};
			ret = bdev->bd_ops->submit(bdev, &req->flush);
			if (ret < 0)
				return ret;
			req->flush_submitted = true;
			req->irreversible = true;
			req->stage = 3;
		}
		if (!io_request_flush_done(req))
			return -EAGAIN;
		return req->flush_result < 0 ? req->flush_result :
		       file->f_inode->i_sb->s_error;
	}
	if (req->offset == -1 && file->f_inode &&
	    S_ISREG(file->f_inode->i_mode) && !req->position_locked) {
		if (!io_position_trylock())
			return -EAGAIN;
		req->position_locked = true;
	}
	if (file->f_op->try_io_pos) {
		loff_t pos = req->offset == -1 ? file->f_pos : req->offset;
		if (req->op == IO_REQUEST_WRITE && (file->f_flags & O_APPEND))
			pos = (loff_t)file->f_inode->i_size;
		ret = file->f_op->try_io_pos(file, req->buffer, req->length,
					   pos, req->op == IO_REQUEST_WRITE);
		if (ret > 0) {
			if (req->offset == -1)
				file->f_pos = pos + ret;
			else
				req->offset = pos + ret;
		}
		return ret;
	}
	if (!file->f_op->try_io)
		return -EOPNOTSUPP;
	return file->f_op->try_io(file, req->buffer, req->length,
				  req->op == IO_REQUEST_WRITE);
}

int io_request_cancel(struct io_request *req)
{
	return req->irreversible &&
	       !(req->flush_submitted && io_request_flush_done(req))
		      ? -EALREADY : 0;
}

bool io_request_finish(struct io_request *req, int result)
{
	if (req->finished)
		return false;
	req->finished = true;
	req->result = result;
	return true;
}

void io_request_release(struct io_request *req)
{
	BUG_ON(req->flush_submitted && !io_request_flush_done(req));
	if (req->position_locked) {
		io_position_unlock();
		req->position_locked = false;
	}
}

static ssize_t io_sync_run(struct io_request *req)
{
	struct wait_deadline deadline = wait_deadline_none();
	size_t done = 0;
	for (;;) {
		struct wait_entry entries[4];
		struct wait_scope scope = {.entries = entries, .capacity = 4};
		wait_outcome_t outcome;
		ssize_t ret;
		int err = wait_scope_begin(&scope,
					   req->flush_submitted ? 0 :
					   WAIT_FLAG_INTERRUPTIBLE,
					   &deadline);
		if (err < 0) {
			if (req->flush_submitted &&
			    !io_request_flush_done(req))
				continue;
			io_request_release(req);
			return done ? (ssize_t)done : err;
		}
		err = wait_scope_prepare(&scope, pgcache_progress_channel(), false);
		if (!err)
			err = wait_scope_prepare(&scope, io_position_channel(), false);
		if (!err && req->file->f_inode)
			err = wait_scope_prepare(&scope,
					&req->file->f_inode->i_lock.wait, false);
		if (!err)
			err = wait_scope_prepare(&scope, &req->completion, false);
		if (err < 0) {
			wait_scope_complete(&scope);
			if (req->flush_submitted &&
			    !io_request_flush_done(req))
				continue;
			io_request_release(req);
			return done ? (ssize_t)done : err;
		}
		if (req->op != IO_REQUEST_FSYNC) {
			req->buffer = (char *)req->buffer + done;
			req->length -= done;
		}
		ret = io_request_progress(req);
		if (req->op != IO_REQUEST_FSYNC) {
			req->buffer = (char *)req->buffer - done;
			req->length += done;
		}
		if (ret != -EAGAIN) {
			wait_scope_complete(&scope);
			if (ret <= 0 || req->op == IO_REQUEST_FSYNC) {
				io_request_release(req);
				return done ? (ssize_t)done : ret;
			}
			done += ret;
			if (done >= req->length)
				break;
			continue;
		}
		err = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		if (err < 0 || outcome == WAIT_OUTCOME_SIGNAL) {
			/* Once a flush has reached the device, the stack request must
			 * stay alive until its IRQ callback retires it. */
			if (req->flush_submitted && !io_request_flush_done(req))
				continue;
			io_request_release(req);
			return done ? (ssize_t)done : err < 0 ? err : -EINTR;
		}
	}
	io_request_release(req);
	return done;
}

ssize_t io_sync_transfer(struct file *file, void *buffer, size_t length,
			loff_t *pos, bool write)
{
	struct io_request req;
	ssize_t ret;
	io_request_init(&req, file, write ? IO_REQUEST_WRITE : IO_REQUEST_READ,
			buffer, length, pos ? *pos : -1, false);
	ret = io_sync_run(&req);
	if (ret > 0 && pos)
		*pos = req.offset;
	return ret;
}

int io_sync_file(struct file *file, bool datasync)
{
	struct io_request req;
	io_request_init(&req, file, IO_REQUEST_FSYNC, NULL, 0, 0, datasync);
	return (int)io_sync_run(&req);
}

ssize_t io_wait_transfer(struct file *file, void *buffer, size_t length,
			 bool write)
{
	struct wait_deadline deadline = wait_deadline_none();
	struct wait_entry entries[2];
	struct io_request req;

	if (!file->f_op || !file->f_op->try_io)
		return -EOPNOTSUPP;
	io_request_init(&req, file,
			write ? IO_REQUEST_WRITE : IO_REQUEST_READ,
			buffer, length, -1, false);
	for (;;) {
		struct wait_scope scope = {.entries = entries, .capacity = 2};
		wait_outcome_t outcome;
		ssize_t ret = wait_scope_begin(&scope, WAIT_FLAG_INTERRUPTIBLE,
					       &deadline);
		if (ret < 0)
			return ret;
		ret = vfs_poll(file, write ? POLLOUT : POLLIN, scope.wait);
		if (ret >= 0)
			ret = io_request_progress(&req);
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

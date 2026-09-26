/* Bounded native asynchronous I/O. The reactor only retries nonblocking
 * transfers and readiness queries; it never calls a blocking VFS operation.
 * ring->lock serializes submission, completion ownership, cancel and close.
 * Source callbacks only wake the reactor, with unsubscribe providing
 * quiescence. A request slot is also its reserved overflow CQE: completion
 * never allocates. */
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/exit.h>
#include <nuvix/fdtable.h>
#include <nuvix/io_buffer.h>
#include <nuvix/io.h>
#include <nuvix/page_cache.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/time.h>
#include <nuvix/timer.h>
#include <nuvix/tools.h>
#include <uapi/io_uring.h>
#include <uapi/uio.h>

#define RING_ENTRIES 64
#define RING_LIMIT   16
#define BUFFER_SLOTS 64
#define IO_VECTORS   16
#define IO_CHUNK     (PAGE_SIZE + 1)
#define IO_POLL_MASK                                                           \
	(POLLIN | POLLOUT | POLLPRI | POLLERR | POLLHUP | POLLRDHUP)

struct ring_memory {
	uint32_t sq_head;
	uint32_t sq_tail;
	uint32_t sq_mask;
	uint32_t sq_entries;
	uint32_t sq_flags;
	uint32_t sq_dropped;
	uint32_t pad[2];
	uint32_t cq_head;
	uint32_t cq_tail;
	uint32_t cq_mask;
	uint32_t cq_entries;
	uint32_t cq_overflow;
	uint32_t cq_flags;
	uint32_t pad2[2];
	uint32_t array[RING_ENTRIES];
	struct io_uring_cqe cqes[2 * RING_ENTRIES];
};

static_assert(sizeof(struct io_uring_sqe) == 64, "SQE ABI");
static_assert(sizeof(struct io_uring_cqe) == 16, "CQE ABI");
static_assert(sizeof(struct io_uring_params) == 120, "params ABI");
static_assert(sizeof(struct ring_memory) <= PAGE_SIZE, "shared ring page");

struct io_ring;
struct ring_request;

struct ring_subscription {
	struct event_subscription event;
	struct io_ring *ring;
};
struct ring_request {
	struct list_head node;
	struct io_ring *ring;
	struct file *file;
	struct io_uring_sqe sqe;
	struct io_uring_cqe cqe;
	struct poll_table poll;
	struct ring_subscription subscriptions[4];
	unsigned int nr_subscriptions;
	struct io_buffer **buffers;
	unsigned int nr_buffers;
	size_t length;
	size_t buffer_offset;
	uint64_t deadline;
	int error;
	bool write;
	bool nonblock;
	bool fixed;
	bool pending;
	bool has_io;
	size_t transferred;
	struct io_request io;
};

struct io_ring {
	refcount_t refs;
	mutex_t lock;
	mutex_t registration_lock;
	struct wait_channel completions;
	spinlock_t cq_lock;
	struct ring_memory *memory;
	struct io_uring_sqe *sqes;
	struct anon_shared *ring_backing;
	struct anon_shared *sqe_backing;
	struct ring_request *requests;
	struct list_head free;
	struct list_head active;
	struct list_head overflow;
	struct task_struct *reactor;
	atomic_t dirty;
	atomic64_t wake_generation;
	struct io_buffer **buffers;
	unsigned int nr_buffers;
	bool buffers_busy;
	bool closing;
	uint32_t entries;
	uint32_t cq_entries;
	uint32_t sq_head;
	uint32_t cq_head;
	uint32_t cq_tail;
	uint32_t dropped;
	char *bounce;
};

static atomic_t ring_count = ATOMIC_INIT(0);

static const struct file_operations ring_fops;

static inline uint32_t load_acquire(uint32_t *value)
{
	return atomic_load_explicit(value, ATOMIC_ORDER_ACQUIRE);
}

static inline void store_release(uint32_t *value, uint32_t data)
{
	atomic_store_explicit(value, data, ATOMIC_ORDER_RELEASE);
}

static void ring_kick(struct io_ring *ring)
{
	atomic_set(&ring->dirty, 1);
	if (ring->reactor)
		(void)sched_wake_event(ring->reactor,
				       atomic64_read(&ring->wake_generation));
}

static void ring_notify(struct event_subscription *event)
{
	struct ring_subscription *sub =
		container_of(event, struct ring_subscription, event);
	ring_kick(sub->ring);
}

static int ring_queue(struct poll_table *table, struct wait_channel *source)
{
	struct ring_request *req =
		container_of(table, struct ring_request, poll);
	struct ring_subscription *sub;
	for (unsigned int i = 0; i < req->nr_subscriptions; i++)
		if (req->subscriptions[i].event.source == source)
			return 0;
	if (req->nr_subscriptions == 4)
		return -E2BIG;
	sub = &req->subscriptions[req->nr_subscriptions++];
	sub->ring = req->ring;
	sub->event.notify = ring_notify;
	event_subscribe(&sub->event, source);
	return 0;
}

static void request_release(struct ring_request *req)
{
	if (req->has_io)
		io_request_release(&req->io);
	for (unsigned int i = 0; i < req->nr_subscriptions; i++)
		event_unsubscribe(&req->subscriptions[i].event);
	req->nr_subscriptions = 0;
	for (unsigned int i = 0; i < req->nr_buffers; i++)
		io_buffer_put(req->buffers[i]);
	kfree(req->buffers);
	req->buffers = NULL;
	req->nr_buffers = 0;
	file_put(req->file);
	req->file = NULL;
}

/* ring mutex held. Private counters, never writable shared masks/tails, bound
 * all indexing. Invalid consumer advances stop reuse rather than lose CQEs. */
static int ring_flush(struct io_ring *ring)
{
	struct ring_memory *mem = ring->memory;
	uint32_t head = load_acquire(&mem->cq_head);
	irq_flags_t flags;
	if (head - ring->cq_head > ring->cq_tail - ring->cq_head)
		return -EINVAL;
	ring->cq_head = head;
	spin_lock_irqsave(&ring->cq_lock, flags);
	while (!list_empty(&ring->overflow) &&
	       ring->cq_tail - head < ring->cq_entries) {
		struct ring_request *req = list_first_entry(
			&ring->overflow, struct ring_request, node);
		mem->cqes[ring->cq_tail & (ring->cq_entries - 1)] = req->cqe;
		ring->cq_tail++;
		list_move_tail(&req->node, &ring->free);
	}
	store_release(&mem->cq_tail, ring->cq_tail);
	store_release(&mem->sq_flags,
		      list_empty(&ring->overflow) ? 0 : IORING_SQ_CQ_OVERFLOW);
	spin_unlock_irqrestore(&ring->cq_lock, flags);
	return 0;
}

static void request_complete(struct ring_request *req, int result)
{
	struct io_ring *ring = req->ring;
	irq_flags_t flags;
	if (req->has_io && !io_request_finish(&req->io, result))
		return;
	req->pending = false;
	request_release(req);
	req->cqe = (struct io_uring_cqe){.user_data = req->sqe.user_data,
					 .res = result};
	spin_lock_irqsave(&ring->cq_lock, flags);
	list_move_tail(&req->node, &ring->overflow);
	spin_unlock_irqrestore(&ring->cq_lock, flags);
	(void)ring_flush(ring);
	wait_channel_wake_all(&ring->completions);
}

static bool supported_opcode(unsigned int op)
{
	switch (op) {
	case IORING_OP_NOP:
	case IORING_OP_READ:
	case IORING_OP_WRITE:
	case IORING_OP_READV:
	case IORING_OP_WRITEV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_FSYNC:
	case IORING_OP_POLL_ADD:
	case IORING_OP_TIMEOUT:
	case IORING_OP_ASYNC_CANCEL:
		return true;
	default:
		return false;
	}
}

static int request_buffers(struct ring_request *req)
{
	struct io_uring_sqe *sqe = &req->sqe;
	struct io_ring *ring = req->ring;
	struct iovec vectors[IO_VECTORS];
	unsigned int count = 1;
	int ret;
	if (req->fixed) {
		struct io_buffer *buffer;
		if (ring->buffers_busy)
			return -EBUSY;
		if (sqe->buf_index >= ring->nr_buffers)
			return -EFAULT;
		buffer = ring->buffers[sqe->buf_index];
		if (!buffer || sqe->addr < buffer->address ||
		    sqe->addr - buffer->address > buffer->length ||
		    sqe->len > buffer->length - (sqe->addr - buffer->address))
			return -EFAULT;
		if (!req->write && !buffer->writable)
			return -EFAULT;
		req->buffers = kmalloc(sizeof(buffer), ALLOC_NOWAIT);
		if (!req->buffers)
			return -ENOMEM;
		io_buffer_get(buffer);
		req->buffers[0] = buffer;
		req->nr_buffers = 1;
		req->buffer_offset = sqe->addr - buffer->address;
		req->length = sqe->len;
		return 0;
	}
	if (sqe->buf_index)
		return -EINVAL;
	if (sqe->opcode == IORING_OP_READV || sqe->opcode == IORING_OP_WRITEV) {
		count = sqe->len;
		if (count > IO_VECTORS)
			return -EINVAL;
		if (copy_from_user(vectors, (void *)sqe->addr,
				   count * sizeof(*vectors)))
			return -EFAULT;
	} else {
		vectors[0] = (struct iovec){.iov_base = (void *)sqe->addr,
					    .iov_len = sqe->len};
	}
	if (!count)
		return 0;
	req->buffers = kmalloc(count * sizeof(*req->buffers), ALLOC_NOWAIT);
	if (!req->buffers)
		return -ENOMEM;
	for (unsigned int i = 0; i < count; i++) {
		if (vectors[i].iov_len > 0x7ffff000UL - req->length)
			return -EINVAL;
		ret = io_buffer_pin(
			current_task()->mm, (uintptr_t)vectors[i].iov_base,
			vectors[i].iov_len, !req->write, &req->buffers[i]);
		if (ret < 0)
			return ret;
		req->nr_buffers++;
		req->length += vectors[i].iov_len;
	}
	return 0;
}

static int request_prepare(struct ring_request *req)
{
	struct io_uring_sqe *sqe = &req->sqe;
	struct timespec ts;
	uint64_t ticks;
	int ret;
	if (!supported_opcode(sqe->opcode))
		return -EOPNOTSUPP;
	if (sqe->flags || sqe->ioprio || sqe->personality || sqe->file_index ||
	    sqe->addr3 || sqe->pad2)
		return -EINVAL;
	switch (sqe->opcode) {
	case IORING_OP_NOP:
		return sqe->rw_flags || sqe->buf_index ? -EINVAL : 0;
	case IORING_OP_TIMEOUT:
		if (sqe->off || sqe->len != 1 || sqe->buf_index ||
		    (sqe->timeout_flags & ~IORING_TIMEOUT_ABS))
			return -EINVAL;
		if (copy_from_user(&ts, (void *)sqe->addr, sizeof(ts)))
			return -EFAULT;
		ret = timespec_to_mtime_delta(&ts, &ticks);
		if (ret < 0)
			return ret;
		req->deadline =
			sqe->timeout_flags & IORING_TIMEOUT_ABS
				? ticks
				: mtime_deadline_after(timer_now(), ticks);
		return 0;
	case IORING_OP_ASYNC_CANCEL:
		return sqe->off || sqe->len || sqe->cancel_flags ||
				       sqe->buf_index
			       ? -EINVAL
			       : 0;
	case IORING_OP_POLL_ADD:
		if (sqe->off || sqe->addr || sqe->len || sqe->buf_index ||
		    (sqe->poll32_events & ~IO_POLL_MASK))
			return -EINVAL;
		break;
	case IORING_OP_FSYNC:
		/* Linux ignores off/len for whole-file fsync. */
		if (sqe->addr || sqe->buf_index ||
		    (sqe->fsync_flags & ~IORING_FSYNC_DATASYNC))
			return -EINVAL;
		break;
	default:
		if (sqe->rw_flags)
			return -EOPNOTSUPP;
		break;
	}
	req->file = fd_get(sqe->fd);
	if (!req->file)
		return -EBADF;
	/* Avoid cross-ring reference cycles; ring fds remain pollable by ppoll.
	 */
	if (req->file->f_op == &ring_fops)
		return -EOPNOTSUPP;
	if (sqe->opcode == IORING_OP_POLL_ADD)
		return 0;
	if (sqe->opcode == IORING_OP_FSYNC) {
		if (!req->file->f_op || !req->file->f_op->try_fsync_prepare)
			return -EOPNOTSUPP;
		io_request_init(&req->io, req->file, IO_REQUEST_FSYNC, NULL, 0,
				0,
				!!(sqe->fsync_flags & IORING_FSYNC_DATASYNC));
		req->has_io = true;
		return 0;
	}
	req->write = sqe->opcode == IORING_OP_WRITE ||
		     sqe->opcode == IORING_OP_WRITEV ||
		     sqe->opcode == IORING_OP_WRITE_FIXED;
	req->fixed = sqe->opcode == IORING_OP_READ_FIXED ||
		     sqe->opcode == IORING_OP_WRITE_FIXED;
	if (!(req->file->f_mode & (req->write ? FMODE_WRITE : FMODE_READ)))
		return -EBADF;
	if (!req->file->f_op ||
	    (!req->file->f_op->try_io && !req->file->f_op->try_io_pos))
		return -EOPNOTSUPP;
	if (req->file->f_op->try_io_pos && sqe->off != UINT64_MAX &&
	    sqe->off > INT64_MAX)
		return -EINVAL;
	/* Native stream backends have no seekable position. */
	if (!req->file->f_op->try_io_pos && sqe->off && sqe->off != UINT64_MAX)
		return -ESPIPE;
	req->nonblock = (req->file->f_flags & O_NONBLOCK) != 0;
	ret = request_buffers(req);
	if (ret < 0)
		return ret;
	io_request_init(&req->io, req->file,
			req->write ? IO_REQUEST_WRITE : IO_REQUEST_READ,
			req->ring->bounce, 0,
			sqe->off == UINT64_MAX ? -1 : (loff_t)sqe->off, false);
	req->has_io = true;
	return 0;
}

static void request_copy(struct ring_request *req, size_t start, size_t length,
			 bool to_user)
{
	size_t done = 0;
	for (unsigned int i = 0; i < req->nr_buffers && done < length; i++) {
		size_t offset = req->fixed ? req->buffer_offset : 0;
		size_t available = req->buffers[i]->length - offset;
		if (start >= available) {
			start -= available;
			continue;
		}
		offset += start;
		start = 0;
		size_t part =
			MIN(length - done, req->buffers[i]->length - offset);
		io_buffer_copy(req->buffers[i], offset,
			       req->ring->bounce + done, part, to_user);
		done += part;
	}
}

static void request_progress(struct ring_request *req)
{
	struct io_ring *ring = req->ring;
	int ret;
	if (req->error) {
		request_complete(req, req->error);
		return;
	}
	switch (req->sqe.opcode) {
	case IORING_OP_NOP:
		request_complete(req, 0);
		return;
	case IORING_OP_TIMEOUT:
		if (timer_now() >= req->deadline)
			request_complete(req, -ETIME);
		return;
	case IORING_OP_ASYNC_CANCEL: {
		struct ring_request *target;
		ret = -ENOENT;
		list_for_each_entry (target, &ring->active, node) {
			if (target != req &&
			    target->sqe.user_data == req->sqe.addr) {
				ret = target->transferred ? -EALREADY
				      : target->has_io
					      ? io_request_cancel(&target->io)
					      : 0;
				if (!ret)
					request_complete(target, -ECANCELED);
				break;
			}
		}
		request_complete(req, ret);
		return;
	}
	case IORING_OP_POLL_ADD:
		ret = vfs_poll_subscribe(req->file, req->sqe.poll32_events,
					 &req->poll);
		if (ret >= 0)
			ret &= req->sqe.poll32_events | POLLERR | POLLHUP;
		if (ret)
			request_complete(req, ret);
		return;
	case IORING_OP_FSYNC:
		(void)ring_queue(&req->poll, pgcache_progress_channel());
		(void)ring_queue(&req->poll, &req->file->f_inode->i_lock.wait);
		(void)ring_queue(&req->poll, &req->io.completion);
		ret = (int)io_request_progress(&req->io);
		if (ret != -EAGAIN)
			request_complete(req, ret);
		return;
	default: {
		if (req->file->f_op->try_io_pos) {
			(void)ring_queue(&req->poll,
					 pgcache_progress_channel());
			(void)ring_queue(&req->poll, io_position_channel());
			(void)ring_queue(&req->poll,
					 &req->file->f_inode->i_lock.wait);
		} else {
			ret = vfs_poll_subscribe(req->file,
						 req->write ? POLLOUT : POLLIN,
						 &req->poll);
			if (ret < 0) {
				request_complete(req, ret);
				return;
			}
		}
		while (req->transferred < req->length) {
			size_t length = MIN(req->length - req->transferred,
					    (size_t)IO_CHUNK);
			if (req->write)
				request_copy(req, req->transferred, length,
					     false);
			req->io.buffer = ring->bounce;
			req->io.length = length;
			ret = (int)io_request_progress(&req->io);
			if (ret == -EAGAIN) {
				if (req->nonblock &&
				    !req->file->f_op->try_io_pos)
					request_complete(
						req,
						req->transferred
							? (int)req->transferred
							: ret);
				return;
			}
			if (ret <= 0) {
				request_complete(req,
						 req->transferred
							 ? (int)req->transferred
							 : ret);
				return;
			}
			if (!req->write)
				request_copy(req, req->transferred, (size_t)ret,
					     true);
			req->transferred += (size_t)ret;
			/* A pipe or console can return a short transfer for its
			 * currently available data. */
			if (!req->file->f_op->try_io_pos &&
			    (size_t)ret < length) {
				request_complete(req, (int)req->transferred);
				return;
			}
		}
		request_complete(req, (int)req->transferred);
	}
	}
}

static void ring_destroy(struct io_ring *ring)
{
	for (unsigned int i = 0; i < ring->nr_buffers; i++)
		io_buffer_put(ring->buffers[i]);
	kfree(ring->buffers);
	anon_shared_put(ring->ring_backing);
	anon_shared_put(ring->sqe_backing);
	if (ring->memory)
		page_put(virt_to_page(ring->memory));
	if (ring->sqes)
		page_put(virt_to_page(ring->sqes));
	kfree(ring->bounce);
	kfree(ring->requests);
	kfree(ring);
	atomic_fetch_sub_order(&ring_count, 1, ATOMIC_ORDER_SEQ_CST);
}

static void ring_put(struct io_ring *ring)
{
	if (refcount_dec_and_test(&ring->refs))
		ring_destroy(ring);
}

static void ring_reactor(void *arg)
{
	struct io_ring *ring = arg;
	for (;;) {
		struct wait_deadline deadline = wait_deadline_none();
		struct wait_scope scope = {};
		struct ring_request *req;
		wait_outcome_t outcome;
		int ret;
		atomic_set(&ring->dirty, 0);
		mutex_lock(&ring->lock);
		if (ring->closing) {
			struct list_head *pos, *next;
			list_for_each_safe (pos, next, &ring->active) {
				req = list_entry(pos, struct ring_request,
						 node);
				if (req->has_io && req->io.flush_submitted &&
				    !io_request_flush_done(&req->io))
					continue;
				list_del_init(&req->node);
				request_release(req);
			}
			if (!list_empty(&ring->active)) {
				mutex_unlock(&ring->lock);
				goto park;
			}
			mutex_unlock(&ring->lock);
			ring_put(ring);
			do_exit(0);
		}
		/* Snapshot pointers: cancel may move any other request to
		 * free/CQ. */
		struct ring_request *pending[2 * RING_ENTRIES];
		unsigned int count = 0;
		list_for_each_entry (req, &ring->active, node)
			pending[count++] = req;
		for (unsigned int i = 0; i < count; i++) {
			if (pending[i]->pending)
				request_progress(pending[i]);
		}
		list_for_each_entry (req, &ring->active, node) {
			if (req->sqe.opcode == IORING_OP_TIMEOUT &&
			    (!deadline.active ||
			     req->deadline < deadline.expires))
				deadline = wait_deadline_at(req->deadline);
		}
		mutex_unlock(&ring->lock);
	park:
		ret = wait_scope_begin(&scope, 0, &deadline);
		if (ret < 0)
			continue;
		atomic64_set(&ring->wake_generation, scope.generation);
		if (!atomic_read(&ring->dirty))
			ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		(void)ret;
	}
}

static int ring_release(struct file *file)
{
	struct io_ring *ring = file->private_data;
	mutex_lock(&ring->lock);
	ring->closing = true;
	ring_kick(ring);
	mutex_unlock(&ring->lock);
	/* The reactor owns final cleanup; no allocation or blocking join. */
	ring_put(ring);
	return 0;
}

static int ring_mmap(struct file *file, uint64_t offset, size_t length,
		     struct anon_shared **backing)
{
	struct io_ring *ring = file->private_data;
	if (length != PAGE_SIZE)
		return -EINVAL;
	if (offset == IORING_OFF_SQ_RING || offset == IORING_OFF_CQ_RING)
		*backing = ring->ring_backing;
	else if (offset == IORING_OFF_SQES)
		*backing = ring->sqe_backing;
	else
		return -EINVAL;
	anon_shared_get(*backing);
	return 0;
}

static int ring_poll(struct file *file, uint32_t events,
		     struct poll_table *table)
{
	struct io_ring *ring = file->private_data;
	irq_flags_t flags;
	int ret;
	(void)events;
	spin_lock_irqsave(&ring->cq_lock, flags);
	ret = poll_wait(table, &ring->completions);
	if (!ret) {
		uint32_t head = load_acquire(&ring->memory->cq_head);
		ret = (!list_empty(&ring->overflow) ||
		       (ring->cq_tail - head &&
			ring->cq_tail - head <= ring->cq_entries))
			      ? POLLIN
			      : 0;
	}
	spin_unlock_irqrestore(&ring->cq_lock, flags);
	return ret;
}

static const struct file_operations ring_fops = {
	.release = ring_release,
	.poll = ring_poll,
	.mmap = ring_mmap,
};

ssize_t sys_io_uring_setup(struct trap_frame *tf)
{
	uint32_t entries = sysarg(tf, 0), size = 1;
	struct io_uring_params *user = (void *)sysarg(tf, 1), params;
	struct io_ring *ring;
	struct file *file;
	int fd;
	if (copy_from_user(&params, user, sizeof(params)))
		return -EFAULT;
	if (!entries || entries > RING_ENTRIES || params.flags ||
	    params.resv[0] || params.resv[1] || params.resv[2])
		return -EINVAL;
	while (size < entries)
		size <<= 1;
	if (atomic_fetch_add_order(&ring_count, 1, ATOMIC_ORDER_SEQ_CST) >=
	    RING_LIMIT) {
		atomic_fetch_sub_order(&ring_count, 1, ATOMIC_ORDER_SEQ_CST);
		return -ENFILE;
	}
	ring = kmalloc(sizeof(*ring), ALLOC_NOWAIT);
	if (!ring) {
		atomic_fetch_sub_order(&ring_count, 1, ATOMIC_ORDER_SEQ_CST);
		return -ENOMEM;
	}
	memset(ring, 0, sizeof(*ring));
	refcount_set(&ring->refs, 1);
	mutex_init(&ring->lock);
	mutex_init(&ring->registration_lock);
	spin_lock_init(&ring->cq_lock);
	wait_channel_init(&ring->completions);
	INIT_LIST_HEAD(&ring->free);
	INIT_LIST_HEAD(&ring->active);
	INIT_LIST_HEAD(&ring->overflow);
	ring->entries = size;
	ring->cq_entries = size * 2;
	ring->memory = get_page(0, ALLOC_NOWAIT);
	ring->sqes = get_page(0, ALLOC_NOWAIT);
	ring->bounce = kmalloc(IO_CHUNK, ALLOC_NOWAIT);
	ring->requests =
		kmalloc(size * 2 * sizeof(*ring->requests), ALLOC_NOWAIT);
	if (!ring->memory || !ring->sqes || !ring->bounce || !ring->requests)
		goto nomem;
	memset(ring->memory, 0, PAGE_SIZE);
	memset(ring->sqes, 0, PAGE_SIZE);
	memset(ring->requests, 0, size * 2 * sizeof(*ring->requests));
	for (unsigned int i = 0; i < size * 2; i++)
		list_add_tail(&ring->requests[i].node, &ring->free);
	ring->ring_backing = anon_shared_from_page(virt_to_page(ring->memory));
	ring->sqe_backing = anon_shared_from_page(virt_to_page(ring->sqes));
	if (!ring->ring_backing || !ring->sqe_backing)
		goto nomem;
	ring->memory->sq_mask = size - 1;
	ring->memory->sq_entries = size;
	ring->memory->cq_mask = size * 2 - 1;
	ring->memory->cq_entries = size * 2;
	memset(&params, 0, sizeof(params));
	params.sq_entries = size;
	params.cq_entries = size * 2;
	params.features = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP |
			  IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_FAST_POLL |
			  IORING_FEAT_EXT_ARG | IORING_FEAT_RW_CUR_POS;
#define OFF(member) offsetof(struct ring_memory, member)
	params.sq_off = (struct io_sqring_offsets){
		.head = OFF(sq_head),
		.tail = OFF(sq_tail),
		.ring_mask = OFF(sq_mask),
		.ring_entries = OFF(sq_entries),
		.flags = OFF(sq_flags),
		.dropped = OFF(sq_dropped),
		.array = OFF(array),
	};
	params.cq_off = (struct io_cqring_offsets){
		.head = OFF(cq_head),
		.tail = OFF(cq_tail),
		.ring_mask = OFF(cq_mask),
		.ring_entries = OFF(cq_entries),
		.overflow = OFF(cq_overflow),
		.cqes = OFF(cqes),
		.flags = OFF(cq_flags),
	};
#undef OFF
	if (copy_to_user(user, &params, sizeof(params))) {
		ring_destroy(ring);
		return -EFAULT;
	}
	file = file_alloc(&ring_fops, FMODE_READ | FMODE_WRITE, ring);
	if (!file)
		goto nomem;
	mutex_lock(&ring->lock);
	refcount_inc(&ring->refs);
	ring->reactor = kernel_thread(ring_reactor, ring);
	mutex_unlock(&ring->lock);
	if (!ring->reactor) {
		ring_put(ring);
		file->f_op = NULL;
		file_put(file);
		goto nomem;
	}
	fd = fd_alloc_flags(file, O_CLOEXEC);
	if (fd < 0)
		file_put(file);
	return fd;
nomem:
	ring_destroy(ring);
	return -ENOMEM;
}

static int ring_submit(struct io_ring *ring, uint32_t count)
{
	uint32_t tail;
	int consumed = 0, ret;
	mutex_lock(&ring->lock);
	ret = ring_flush(ring);
	if (ret < 0)
		goto out;
	tail = load_acquire(&ring->memory->sq_tail);
	if (tail - ring->sq_head > ring->entries) {
		ret = -EINVAL;
		goto out;
	}
	while (count && ring->sq_head != tail) {
		struct ring_request *req;
		uint32_t index;
		if (list_empty(&ring->free)) {
			ret = -EBUSY;
			break;
		}
		index = load_acquire(&ring->memory->array[ring->sq_head &
							  (ring->entries - 1)]);
		if (index >= ring->entries) {
			store_release(&ring->memory->sq_dropped,
				      ++ring->dropped);
			store_release(&ring->memory->sq_head, ++ring->sq_head);
			consumed++;
			count--;
			continue;
		}
		req = list_first_entry(&ring->free, struct ring_request, node);
		list_del_init(&req->node);
		memset(req, 0, sizeof(*req));
		req->ring = ring;
		req->poll.queue = ring_queue;
		memcpy(&req->sqe, &ring->sqes[index], sizeof(req->sqe));
		req->error = request_prepare(req);
		req->pending = true;
		list_add_tail(&req->node, &ring->active);
		store_release(&ring->memory->sq_head, ++ring->sq_head);
		/* An immediately executable request need not wait for the
		 * reactor. Implicit regular-file positions stay with the
		 * reactor task if a page producer makes them wait. */
		if (req->error ||
		    (req->sqe.opcode != IORING_OP_ASYNC_CANCEL &&
		     !req->has_io) ||
		    (req->sqe.opcode != IORING_OP_FSYNC &&
		     req->sqe.opcode != IORING_OP_ASYNC_CANCEL &&
		     (!req->file->f_op->try_io_pos ||
		      req->sqe.off != UINT64_MAX)))
			request_progress(req);
		consumed++;
		count--;
	}
	ring_kick(ring);
out:
	mutex_unlock(&ring->lock);
	return consumed ? consumed : ret;
}

static int ring_wait(struct io_ring *ring, uint32_t minimum,
		     struct wait_deadline *deadline)
{
	for (;;) {
		struct wait_scope scope = {};
		wait_outcome_t outcome;
		int ret;
		bool ready;
		irq_flags_t flags;
		mutex_lock(&ring->lock);
		ret = ring_flush(ring);
		mutex_unlock(&ring->lock);
		if (ret < 0 || !minimum)
			return ret;
		ret = wait_scope_begin(&scope, WAIT_FLAG_INTERRUPTIBLE,
				       deadline);
		if (ret < 0)
			return ret;
		/* Registration precedes the acquire CQ-tail check. */
		ret = wait_scope_prepare(&scope, &ring->completions, false);
		spin_lock_irqsave(&ring->cq_lock, flags);
		uint32_t available =
			ring->cq_tail - load_acquire(&ring->memory->cq_head);
		ready = available <= ring->cq_entries && available >= minimum;
		spin_unlock_irqrestore(&ring->cq_lock, flags);
		if (!ret && !ready)
			ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		if (ret < 0 || ready)
			return ret;
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
		if (outcome == WAIT_OUTCOME_TIMEOUT)
			return -ETIME;
	}
}

ssize_t sys_io_uring_enter(struct trap_frame *tf)
{
	struct file *file = fd_get((int)sysarg(tf, 0));
	uint32_t submit = sysarg(tf, 1), minimum = sysarg(tf, 2),
		 flags = sysarg(tf, 3);
	void *arg = (void *)sysarg(tf, 4);
	size_t size = sysarg(tf, 5);
	struct wait_deadline deadline = wait_deadline_none();
	struct io_uring_getevents_arg ext;
	struct timespec ts;
	struct io_ring *ring;
	uint64_t mask = 0, oldmask = 0;
	bool change_mask = false;
	int ret = -EBADF, consumed = 0;
	if (!file)
		return ret;
	if (file->f_op != &ring_fops) {
		ret = -EINVAL;
		goto out;
	}
	ring = file->private_data;
	if (flags & ~(IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG)) {
		ret = -EINVAL;
		goto out;
	}
	if (flags & IORING_ENTER_EXT_ARG) {
		if (size != sizeof(ext)) {
			ret = -EINVAL;
			goto out;
		}
		if (copy_from_user(&ext, arg, sizeof(ext))) {
			ret = -EFAULT;
			goto out;
		}
		if (ext.pad) {
			ret = -EINVAL;
			goto out;
		}
		if (ext.ts) {
			if (copy_from_user(&ts, (void *)ext.ts, sizeof(ts))) {
				ret = -EFAULT;
				goto out;
			}
			ret = mtime_deadline_from_timespec(&ts, &deadline);
			if (ret < 0)
				goto out;
		}
		arg = (void *)ext.sigmask;
		size = ext.sigmask_sz;
	}
	if (arg) {
		if (size != sizeof(mask)) {
			ret = -EINVAL;
			goto out;
		}
		if (copy_from_user(&mask, arg, sizeof(mask))) {
			ret = -EFAULT;
			goto out;
		}
		change_mask = true;
	}
	consumed = ring_submit(ring, submit);
	if (consumed < 0) {
		ret = consumed;
		goto out;
	}
	ret = 0;
	if (flags & IORING_ENTER_GETEVENTS) {
		if (minimum > ring->cq_entries)
			minimum = ring->cq_entries;
		if (change_mask) {
			oldmask = sig_blocked_mask(current_task());
			sig_set_mask(current_task(), mask);
		}
		ret = ring_wait(ring, minimum, &deadline);
		if (change_mask) {
			if (ret == -EINTR)
				sig_defer_mask_restore(current_task(), oldmask);
			else
				sig_set_mask(current_task(), oldmask);
		}
	}
	if (consumed)
		ret = consumed;
out:
	file_put(file);
	return ret;
}

static int ring_probe(void *arg, unsigned int count)
{
	unsigned char storage[sizeof(struct io_uring_probe) +
			      256 * sizeof(struct io_uring_probe_op)];
	struct io_uring_probe *probe = (void *)storage;
	size_t bytes;
	if (count > 256)
		return -EINVAL;
	bytes = sizeof(*probe) + count * sizeof(probe->ops[0]);
	if (copy_from_user(probe, arg, bytes))
		return -EFAULT;
	for (size_t i = 0; i < bytes; i++)
		if (storage[i])
			return -EINVAL;
	memset(storage, 0, sizeof(storage));
	probe->last_op = IORING_OP_WRITE;
	probe->ops_len = MIN(count, (unsigned int)IORING_OP_WRITE + 1);
	for (unsigned int i = 0; i < probe->ops_len; i++) {
		probe->ops[i].op = i;
		probe->ops[i].flags =
			supported_opcode(i) ? IO_URING_OP_SUPPORTED : 0;
	}
	return copy_to_user(arg, probe, bytes) ? -EFAULT : 0;
}

/* Registration changes stop new fixed requests, wait without the ring lock,
 * then publish atomically. Signals leave the original table intact. */
static int buffers_quiesce(struct io_ring *ring)
{
	struct wait_deadline deadline = wait_deadline_none();
	for (;;) {
		struct wait_scope scope = {};
		wait_outcome_t outcome;
		bool busy = false;
		int ret = wait_scope_begin(&scope, WAIT_FLAG_INTERRUPTIBLE,
					   &deadline);
		if (ret < 0)
			return ret;
		ret = wait_scope_prepare(&scope, &ring->completions, false);
		/* Resource references are atomic; table is held by
		 * registration_lock. */
		for (unsigned int i = 0; i < ring->nr_buffers; i++)
			if (refcount_read(&ring->buffers[i]->refs) > 1)
				busy = true;
		if (!ret && busy)
			ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		if (ret < 0 || !busy)
			return ret;
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
	}
}

static int buffers_register(struct io_ring *ring, unsigned int opcode,
			    void *arg, unsigned int size)
{
	struct io_buffer **prepared = NULL;
	struct io_uring_rsrc_register reg;
	struct io_uring_rsrc_update2 update;
	struct iovec vectors[BUFFER_SLOTS];
	uint64_t data = 0;
	unsigned int count = 0, offset = 0, pinned = 0;
	int ret = 0;
	if (opcode == IORING_REGISTER_BUFFERS2) {
		if (size != sizeof(reg))
			return -EINVAL;
		if (copy_from_user(&reg, arg, sizeof(reg)))
			return -EFAULT;
		if (reg.flags || reg.resv2 || reg.tags)
			return -EINVAL;
		count = reg.nr;
		data = reg.data;
	} else if (opcode == IORING_REGISTER_BUFFERS_UPDATE) {
		if (size != sizeof(update))
			return -EINVAL;
		if (copy_from_user(&update, arg, sizeof(update)))
			return -EFAULT;
		if (update.resv || update.resv2 || update.tags)
			return -EINVAL;
		count = update.nr;
		offset = update.offset;
		data = update.data;
	} else if (arg || size) {
		return -EINVAL;
	}
	if (opcode != IORING_UNREGISTER_BUFFERS &&
	    (!count || count > BUFFER_SLOTS))
		return -EINVAL;
	mutex_lock(&ring->registration_lock);
	mutex_lock(&ring->lock);
	if (opcode == IORING_REGISTER_BUFFERS2 && ring->buffers)
		ret = -EBUSY;
	else if (opcode != IORING_REGISTER_BUFFERS2 && !ring->buffers)
		ret = -ENXIO;
	else if (opcode == IORING_REGISTER_BUFFERS_UPDATE &&
		 (offset > ring->nr_buffers ||
		  count > ring->nr_buffers - offset))
		ret = -EINVAL;
	mutex_unlock(&ring->lock);
	if (ret < 0)
		goto out;
	if (count) {
		if (copy_from_user(vectors, (void *)data,
				   count * sizeof(*vectors))) {
			ret = -EFAULT;
			goto out;
		}
		prepared = kmalloc(count * sizeof(*prepared), ALLOC_NOWAIT);
		if (!prepared) {
			ret = -ENOMEM;
			goto out;
		}
		for (; pinned < count; pinned++) {
			if (!vectors[pinned].iov_len) {
				ret = -EFAULT;
				goto out;
			}
			ret = io_buffer_pin(current_task()->mm,
					    (uintptr_t)vectors[pinned].iov_base,
					    vectors[pinned].iov_len, true,
					    &prepared[pinned]);
			if (ret < 0)
				goto out;
		}
	}
	mutex_lock(&ring->lock);
	ring->buffers_busy = true;
	mutex_unlock(&ring->lock);
	ret = buffers_quiesce(ring);
	mutex_lock(&ring->lock);
	if (!ret) {
		if (opcode == IORING_REGISTER_BUFFERS2) {
			ring->buffers = prepared;
			ring->nr_buffers = count;
			prepared = NULL;
			pinned = 0;
		} else if (opcode == IORING_UNREGISTER_BUFFERS) {
			for (unsigned int i = 0; i < ring->nr_buffers; i++)
				io_buffer_put(ring->buffers[i]);
			kfree(ring->buffers);
			ring->buffers = NULL;
			ring->nr_buffers = 0;
		} else {
			for (unsigned int i = 0; i < count; i++) {
				io_buffer_put(ring->buffers[offset + i]);
				ring->buffers[offset + i] = prepared[i];
			}
			pinned = 0;
			ret = count;
		}
	}
	ring->buffers_busy = false;
	mutex_unlock(&ring->lock);
out:
	for (unsigned int i = 0; i < pinned; i++)
		io_buffer_put(prepared[i]);
	kfree(prepared);
	mutex_unlock(&ring->registration_lock);
	return ret;
}

ssize_t sys_io_uring_register(struct trap_frame *tf)
{
	struct file *file = fd_get((int)sysarg(tf, 0));
	unsigned int opcode = sysarg(tf, 1), count = sysarg(tf, 3);
	void *arg = (void *)sysarg(tf, 2);
	int ret;
	if (!file)
		return -EBADF;
	if (file->f_op != &ring_fops)
		ret = -EINVAL;
	else if (opcode == IORING_REGISTER_PROBE)
		ret = ring_probe(arg, count);
	else if (opcode == IORING_REGISTER_BUFFERS2 ||
		 opcode == IORING_REGISTER_BUFFERS_UPDATE ||
		 opcode == IORING_UNREGISTER_BUFFERS)
		ret = buffers_register(file->private_data, opcode, arg, count);
	else
		ret = -EINVAL;
	file_put(file);
	return ret;
}

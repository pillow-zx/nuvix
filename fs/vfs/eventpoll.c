/*
 * fs/vfs/eventpoll.c - eventpoll file implementation
 */

#include <nuvix/cleanup.h>
#include <nuvix/errno.h>
#include <nuvix/eventpoll.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/list.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/task.h>
#include <nuvix/tools.h>
#include <nuvix/wait.h>
#include <uapi/poll.h>

struct epitem {
	struct list_head node;
	struct file *file;
	struct epoll_event event;
	int fd;
};

struct eventpoll {
	spinlock_t lock;
	struct list_head items;
	struct wait_channel waitq;
	uint64_t generation;
};

struct epoll_snapshot_item {
	struct file *file;
	struct epoll_event event;
	int fd;
};

struct epoll_scan_ctx {
	struct eventpoll *ep;
	struct epoll_snapshot_item *items;
	size_t nr_items;
	uint64_t generation;
	struct epoll_event *events;
	size_t maxevents;
	size_t nr_ready;
	bool generation_changed;
};

CLEANUP_DEFINE(eventpoll, struct eventpoll *, if (_T) kfree(_T));

static int eventpoll_release(struct file *file);
static int eventpoll_poll(struct file *file, uint32_t events,
			  struct task_wait *wait);

static const struct file_operations eventpoll_fops = {
	.poll = eventpoll_poll,
	.release = eventpoll_release,
};

bool eventpoll_file(struct file *file)
{
	return file && file->f_op == &eventpoll_fops;
}

__must_check __pure
static bool eventpoll_events_ok(uint32_t events)
{
	const uint32_t supported = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR |
				   EPOLLHUP | EPOLLRDNORM | EPOLLRDBAND |
				   EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG |
				   EPOLLRDHUP;

	return (events & ~supported) == 0;
}

__must_check __pure
static uint32_t epoll_res(uint32_t events, uint32_t requested)
{
	uint32_t res = events;

	if ((requested & EPOLLRDNORM) && (events & EPOLLIN))
		res |= EPOLLRDNORM;
	if ((requested & EPOLLRDBAND) && (events & EPOLLIN))
		res |= EPOLLRDBAND;
	if ((requested & EPOLLWRNORM) && (events & EPOLLOUT))
		res |= EPOLLWRNORM;
	if ((requested & EPOLLWRBAND) && (events & EPOLLOUT))
		res |= EPOLLWRBAND;

	return res;
}

__must_check __pure
static uint32_t epoll_poll_events(struct file *file, const struct epoll_event *event)
{
	uint32_t events = 0;

	if (!file || !event)
		return 0;
	if (file->f_mode & FMODE_READ)
		events |= POLLIN;
	if (file->f_mode & FMODE_WRITE)
		events |= POLLOUT;
	if (event->events & EPOLLPRI)
		events |= POLLPRI;

	if (event->events & (EPOLLRDNORM | EPOLLRDBAND | EPOLLRDHUP))
		events |= POLLIN;
	if (event->events & (EPOLLWRNORM | EPOLLWRBAND))
		events |= POLLOUT;

	return events;
}

__must_check __pure
static uint32_t epoll_result_events(const struct epoll_event *event, uint32_t mask)
{
	uint32_t events = 0;
	uint32_t wanted;

	if (!event)
		return 0;
	if (mask & POLLIN)
		events |= EPOLLIN;
	if (mask & POLLOUT)
		events |= EPOLLOUT;
	if (mask & POLLPRI)
		events |= EPOLLPRI;
	if (mask & POLLERR)
		events |= EPOLLERR;
	if (mask & POLLHUP)
		events |= EPOLLHUP;
	if (mask & POLLNVAL)
		events |= EPOLLNVAL;

	events = epoll_res(events, event->events);
	wanted = event->events &
		 (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDNORM | EPOLLRDBAND |
		  EPOLLWRNORM | EPOLLWRBAND);
	return events & (wanted | EPOLLERR | EPOLLHUP | EPOLLNVAL);
}

static struct epitem *eventpoll_find_locked(struct eventpoll *ep, int fd,
					    struct file *file)
{
	struct epitem *item;

	if (!ep || !file)
		return NULL;

	list_for_each_entry (item, &ep->items, node) {
		if (item->fd == fd && item->file == file)
			return item;
	}

	return NULL;
}

static int eventpoll_scan(struct task_wait *wait, void *arg)
{
	struct epoll_scan_ctx *ctx = arg;
	irq_flags_t flags;

	if (!ctx || !ctx->ep)
		return -EINVAL;

	ctx->nr_ready = 0;
	ctx->generation_changed = false;
	spin_lock_irqsave(&ctx->ep->lock, &flags);
	if (wait) {
		int ret = wait_prepare(wait, &ctx->ep->waitq, false);

		if (ret < 0) {
			spin_unlock_irqrestore(&ctx->ep->lock, flags);
			return ret;
		}
	}
	ctx->generation_changed = ctx->generation != ctx->ep->generation;
	spin_unlock_irqrestore(&ctx->ep->lock, flags);
	if (ctx->generation_changed)
		return 1;

	for (size_t i = 0; i < ctx->nr_items; i++) {
		struct epoll_snapshot_item *item = &ctx->items[i];
		int mask;
		uint32_t events;

		mask = vfs_poll(item->file,
				epoll_poll_events(item->file, &item->event),
				wait);
		if (mask < 0)
			return mask;
		events = epoll_result_events(&item->event, (uint32_t)mask);
		if (!events)
			continue;
		if (ctx->nr_ready >= ctx->maxevents)
			continue;

		ctx->events[ctx->nr_ready].events = events;
		ctx->events[ctx->nr_ready].data = item->event.data;
		ctx->nr_ready++;
	}

	return (int)ctx->nr_ready;
}

static int eventpoll_poll(struct file *file, uint32_t events,
				  struct task_wait *wait)
{
	struct eventpoll *ep = file ? file->private_data : NULL;
	irq_flags_t flags;
	int ret;

	if (!ep)
		return POLLERR;

	if (wait && (events & (POLLIN | POLLOUT))) {
		spin_lock_irqsave(&ep->lock, &flags);
		ret = wait_prepare(wait, &ep->waitq, false);
		spin_unlock_irqrestore(&ep->lock, flags);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void epoll_snapshot_put(struct epoll_scan_ctx *ctx)
{
	if (!ctx || !ctx->items)
		return;

	for (size_t i = 0; i < ctx->nr_items; i++)
		file_put(ctx->items[i].file);
	ctx->nr_items = 0;
}

static int epoll_snapshot_get(struct eventpoll *ep,
			      struct epoll_scan_ctx *ctx)
{
	struct epitem *item;
	irq_flags_t flags;
	size_t nr_items = 0;

	if (!ep || !ctx || !ctx->items)
		return -EINVAL;

	spin_lock_irqsave(&ep->lock, &flags);
	ctx->generation = ep->generation;
	list_for_each_entry (item, &ep->items, node) {
		if (nr_items >= NR_OPEN) {
			spin_unlock_irqrestore(&ep->lock, flags);
			ctx->nr_items = nr_items;
			epoll_snapshot_put(ctx);
			return -E2BIG;
		}

		file_get(item->file);
		ctx->items[nr_items].file = item->file;
		ctx->items[nr_items].event = item->event;
		ctx->items[nr_items].fd = item->fd;
		nr_items++;
	}
	spin_unlock_irqrestore(&ep->lock, flags);

	ctx->nr_items = nr_items;
	return 0;
}

static int eventpoll_release(struct file *file)
{
	struct eventpoll *ep;
	LIST_HEAD(items);
	irq_flags_t flags;

	if (!file)
		return 0;

	ep = file->private_data;
	file->private_data = NULL;
	if (!ep)
		return 0;

	spin_lock_irqsave(&ep->lock, &flags);
	while (!list_empty(&ep->items))
		list_move_tail(ep->items.next, &items);
	ep->generation++;
	spin_unlock_irqrestore(&ep->lock, flags);

	while (!list_empty(&items)) {
		struct list_head *node = items.next;
		struct epitem *item = list_entry(node, struct epitem, node);

		list_del_init(node);
		file_put(item->file);
		kfree(item);
	}
	kfree(ep);
	return 0;
}

struct file *eventpoll_file_alloc(void)
{
	struct eventpoll *ep __cleanup_with(eventpoll) = NULL;
	struct file *file;

	ep = kmalloc(sizeof(*ep), ALLOC_NOWAIT);
	if (!ep)
		return NULL;

	spin_lock_init(&ep->lock, LOCK_RANK_WAIT_CHANNEL);
	INIT_LIST_HEAD(&ep->items);
	wait_channel_init(&ep->waitq);
	ep->generation = 0;

	file = file_alloc(&eventpoll_fops, FMODE_READ | FMODE_WRITE, ep);
	if (!file)
		return NULL;

	cleanup_forget_ptr(ep);
	return file;
}

int eventpoll_ctl(struct file *epfile, int op, int fd, struct file *file,
		  const struct epoll_event *event)
{
	struct eventpoll *ep;
	struct epitem *item;
	struct epitem *found;
	irq_flags_t flags;

	if (!eventpoll_file(epfile))
		return -EINVAL;
	if (!file)
		return -EBADF;
	if (eventpoll_file(file))
		return -EINVAL;
	if (!file->f_op || !file->f_op->poll)
		return -EPERM;

	ep = epfile->private_data;
	if (!ep)
		return -EINVAL;

	switch (op) {
	case EPOLL_CTL_ADD:
		if (!event)
			return -EFAULT;
		if (!eventpoll_events_ok(event->events))
			return -EINVAL;

	item = kmalloc(sizeof(*item), ALLOC_NOWAIT);
		if (!item)
			return -ENOMEM;

		INIT_LIST_HEAD(&item->node);
		item->file = file;
		item->event = *event;
		item->fd = fd;

		spin_lock_irqsave(&ep->lock, &flags);
		if (eventpoll_find_locked(ep, fd, file)) {
			spin_unlock_irqrestore(&ep->lock, flags);
			kfree(item);
			return -EEXIST;
		}
		file_get(file);
		list_add_tail(&item->node, &ep->items);
		ep->generation++;
		spin_unlock_irqrestore(&ep->lock, flags);
		wait_channel_wake_all(&ep->waitq);
		return 0;

	case EPOLL_CTL_MOD:
		if (!event)
			return -EFAULT;
		if (!eventpoll_events_ok(event->events))
			return -EINVAL;

		spin_lock_irqsave(&ep->lock, &flags);
		found = eventpoll_find_locked(ep, fd, file);
		if (!found) {
			spin_unlock_irqrestore(&ep->lock, flags);
			return -ENOENT;
		}
		found->event = *event;
		ep->generation++;
		spin_unlock_irqrestore(&ep->lock, flags);
		wait_channel_wake_all(&ep->waitq);
		return 0;

	case EPOLL_CTL_DEL:
		spin_lock_irqsave(&ep->lock, &flags);
		found = eventpoll_find_locked(ep, fd, file);
		if (!found) {
			spin_unlock_irqrestore(&ep->lock, flags);
			return -ENOENT;
		}

		list_del_init(&found->node);
		ep->generation++;
		spin_unlock_irqrestore(&ep->lock, flags);
		file_put(found->file);
		kfree(found);
		wait_channel_wake_all(&ep->waitq);
		return 0;
	}

	return -EINVAL;
}

int eventpoll_wait(struct file *epfile, struct epoll_event *events,
		   int maxevents, const struct wait_deadline *deadline)
{
	struct epoll_snapshot_item snapshot[NR_OPEN] = {};
	struct epoll_scan_ctx scan_ctx;
	struct task_wait *wait = &current_task()->wait;
	struct eventpoll *ep;
	size_t scan_limit;
	int ret;

	if (!eventpoll_file(epfile))
		return -EINVAL;
	if (!events || maxevents <= 0 || !deadline)
		return -EINVAL;

	ep = epfile->private_data;
	if (!ep)
		return -EINVAL;

	scan_limit = (size_t)maxevents;
	if (scan_limit > ARRLEN(snapshot))
		scan_limit = ARRLEN(snapshot);

	scan_ctx.ep = ep;
	scan_ctx.items = snapshot;
	scan_ctx.nr_items = 0;
	scan_ctx.events = events;
	scan_ctx.maxevents = scan_limit;
	scan_ctx.nr_ready = 0;
	scan_ctx.generation_changed = false;
	while (true) {
		wait_outcome_t outcome;

		ret = epoll_snapshot_get(ep, &scan_ctx);
		if (ret < 0)
			break;
		ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, deadline);
		if (ret == 0)
			ret = eventpoll_scan(wait, &scan_ctx);
		if (ret == 0)
			ret = wait_block(wait, &outcome);
		wait_finish(wait);
		epoll_snapshot_put(&scan_ctx);
		if (ret < 0)
			break;
		if (ret > 0) {
			if (!scan_ctx.generation_changed)
				return (int)scan_ctx.nr_ready;
			continue;
		}
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
		if (outcome == WAIT_OUTCOME_TIMEOUT)
			return 0;
		if (outcome != WAIT_OUTCOME_EVENT)
			return -EINVAL;
	}

	return ret;
}

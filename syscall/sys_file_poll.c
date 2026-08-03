/*
 * syscall/sys_file_poll.c - poll/select/epoll 系统调用
 */

#include <kernel/fdtable.h>
#include <kernel/cleanup.h>
#include <kernel/eventpoll.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/signal.h>
#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/page.h>
#include <kernel/trap.h>
#include <kernel/time.h>
#include <kernel/tools.h>
#include <kernel/wait.h>
#include <uapi/eventpoll.h>
#include <uapi/poll.h>
#include <uapi/select.h>

struct ppoll_scan_ctx {
	struct pollfd *fds;
	struct file **files;
	size_t nfds;
	int ready;
};

struct pselect_scan_ctx {
	const fd_set *in_readfds;
	const fd_set *in_writefds;
	const fd_set *in_exceptfds;
	fd_set *out_readfds;
	fd_set *out_writefds;
	fd_set *out_exceptfds;
	struct file **files;
	size_t nfds;
	int ready;
};

struct poll_sigmask_guard {
	struct task_struct *task;
	uint64_t old_blocked;
	bool active;
};

CLEANUP_DEFINE(poll_sigmask_restore, struct poll_sigmask_guard,
	       if (_T.active) signal_set_blocked_mask(_T.task, _T.old_blocked);)

static_assert(NR_OPEN <= __FD_SETSIZE, "NR_OPEN exceeds fd_set ABI limit");
static_assert(EPOLL_CLOEXEC == O_CLOEXEC, "epoll cloexec flag ABI mismatch");
static_assert(sizeof(struct pollfd) == 8, "pollfd ABI layout mismatch");
static_assert(sizeof(struct epoll_event) == 16,
	      "epoll_event ABI layout mismatch");
static_assert(offsetof(struct epoll_event, data) == 8,
	      "epoll_event data ABI offset mismatch");

__must_check __pure
static size_t sys_fdset_nwords(size_t nfds)
{
	if (nfds == 0)
		return 0;

	return (nfds + __NFDBITS - 1) / __NFDBITS;
}

__must_check __pure
static size_t sys_fdset_nbytes(size_t nfds)
{
	return sys_fdset_nwords(nfds) * sizeof(unsigned long);
}

__must_check __pure
static bool sys_epoll_create1_flags_ok(int flags)
{
	return (flags & ~EPOLL_CLOEXEC) == 0;
}

__must_check __pure
static bool sys_epoll_op_valid(int op)
{
	return op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD ||
	       op == EPOLL_CTL_DEL;
}

__must_check __pure
static bool sys_epoll_wait_sigmask_ok(const unsigned long *usigmask, size_t sigsetsize)
{
	if (usigmask)
		return sigsetsize == sizeof(unsigned long);

	return sigsetsize == 0 || sigsetsize == sizeof(unsigned long);
}

__must_check __pure
static bool sys_fdset_test(const fd_set *set, int fd)
{
	return set && FD_ISSET(fd, set);
}

static void sys_fdset_assign(fd_set *set, int fd, bool ready)
{
	if (!set)
		return;

	if (ready)
		FD_SET(fd, set);
	else
		FD_CLR(fd, set);
}

__must_check __pure
static uint32_t poll_req(uint32_t events)
{
	uint32_t req = events;

	if (events & (POLLRDNORM | POLLRDBAND))
		req |= POLLIN;
	if (events & (POLLWRNORM | POLLWRBAND))
		req |= POLLOUT;
	if (events & POLLRDHUP)
		req |= POLLIN;

	return req;
}

__must_check __pure
static uint32_t poll_res(uint32_t mask, uint32_t requested)
{
	uint32_t res = mask;

	if ((requested & POLLRDNORM) && (mask & POLLIN))
		res |= POLLRDNORM;
	if ((requested & POLLRDBAND) && (mask & POLLIN))
		res |= POLLRDBAND;
	if ((requested & POLLWRNORM) && (mask & POLLOUT))
		res |= POLLWRNORM;
	if ((requested & POLLWRBAND) && (mask & POLLOUT))
		res |= POLLWRBAND;

	return res;
}

static int poll_apply_sigmask(const unsigned long *usigmask, size_t sigsetsize,
			      struct poll_sigmask_guard *guard)
{
	unsigned long new_mask;

	if (!guard)
		return -EINVAL;

	guard->task = current_task();
	guard->old_blocked = 0;
	guard->active = false;

	if (!usigmask) {
		if (sigsetsize != 0 && sigsetsize != sizeof(uint64_t))
			return -EINVAL;
		return 0;
	}
	if (sigsetsize != sizeof(uint64_t))
		return -EINVAL;
	if (copy_from_user(&new_mask, usigmask, sizeof(new_mask)) != 0)
		return -EFAULT;

	guard->old_blocked = signal_blocked_mask(current_task());
	signal_set_blocked_mask(current_task(), new_mask);
	guard->active = true;
	return 0;
}

static void poll_defer_sigmask_restore(struct poll_sigmask_guard *guard)
{
	if (!guard || !guard->active)
		return;

	signal_defer_mask_restore(guard->task, guard->old_blocked);
	guard->active = false;
}

static int pselect_copy_sigmask_args(const struct pselect6_sigmask *upack,
				     const unsigned long **usigmask,
				     size_t *sigsetsize)
{
	struct pselect6_sigmask pack;

	*usigmask = NULL;
	*sigsetsize = 0;
	if (!upack)
		return 0;
	if (copy_from_user(&pack, upack, sizeof(pack)) != 0)
		return -EFAULT;

	*usigmask = pack.ss;
	*sigsetsize = (size_t)pack.ss_len;
	return 0;
}

static int pselect_copy_fdset(fd_set *dst, const fd_set *usrc, size_t bytes)
{
	memset(dst, 0, sizeof(*dst));
	if (!usrc || bytes == 0)
		return 0;
	if (copy_from_user(dst, usrc, bytes) != 0)
		return -EFAULT;

	return 0;
}

static int pselect_copy_result_fdset(fd_set *udst, const fd_set *src,
				     size_t bytes)
{
	if (!udst || bytes == 0)
		return 0;
	if (copy_to_user(udst, src, bytes) != 0)
		return -EFAULT;

	return 0;
}

static void poll_file_snapshot_put(struct file **files, size_t nr_files)
{
	if (!files)
		return;

	for (size_t i = 0; i < nr_files; i++) {
		if (files[i])
			file_put(files[i]);
	}
}

static int poll_wait(struct wait_request *source,
		     const struct wait_deadline *deadline, int *ready)
{
	wait_outcome_t outcome;
	int ret;

	ret = wait_for_interruptible(source, deadline, &outcome);
	if (ret < 0)
		return ret;
	if (outcome == WAIT_OUTCOME_SIGNAL)
		return -EINTR;
	if (outcome == WAIT_OUTCOME_TIMEOUT)
		return 0;
	if (outcome != WAIT_OUTCOME_EVENT)
		return -EINVAL;

	return ready ? *ready : 1;
}

static int ppoll_scan(struct wait_session *session, void *arg)
{
	struct ppoll_scan_ctx *ctx = arg;
	int ready = 0;

	for (size_t i = 0; i < ctx->nfds; i++) {
		struct file *file = ctx->files[i];
		int mask;

		ctx->fds[i].revents = 0;
		if (ctx->fds[i].fd < 0)
			continue;

		if (!file) {
			ctx->fds[i].revents = POLLNVAL;
			ready++;
			continue;
		}

		mask = vfs_poll(file, poll_req((uint32_t)ctx->fds[i].events),
				session);
		if (mask < 0)
			return mask;
		mask = (int)poll_res((uint32_t)mask,
				     (uint32_t)ctx->fds[i].events);
		ctx->fds[i].revents =
			(int16_t)(mask & (ctx->fds[i].events | POLLERR |
					  POLLHUP | POLLNVAL));
		if (ctx->fds[i].revents)
			ready++;
	}

	ctx->ready = ready;
	return ready;
}

static int pselect_scan(struct wait_session *session, void *arg)
{
	struct pselect_scan_ctx *ctx = arg;
	int ready = 0;

	for (size_t fd = 0; fd < ctx->nfds; fd++) {
		struct file *file;
		uint32_t events = 0;
		int mask;
		bool read_ready;
		bool write_ready;
		bool except_ready;

		if (sys_fdset_test(ctx->in_readfds, (int)fd))
			events |= POLLIN;
		if (sys_fdset_test(ctx->in_writefds, (int)fd))
			events |= POLLOUT;
		if (sys_fdset_test(ctx->in_exceptfds, (int)fd))
			events |= POLLPRI;
		if (!events)
			continue;

		file = ctx->files[fd];
		if (!file)
			return -EBADF;

		mask = vfs_poll(file, events, session);
		if (mask < 0)
			return mask;

		read_ready = (mask & (POLLIN | POLLERR | POLLHUP)) != 0;
		write_ready = (mask & (POLLOUT | POLLERR)) != 0;
		except_ready = (mask & POLLPRI) != 0;

		sys_fdset_assign(ctx->out_readfds, (int)fd, read_ready);
		sys_fdset_assign(ctx->out_writefds, (int)fd, write_ready);
		sys_fdset_assign(ctx->out_exceptfds, (int)fd, except_ready);

		ready += read_ready;
		ready += write_ready;
		ready += except_ready;
	}

	ctx->ready = ready;
	return ready;
}

/*
 * SYSCALL_SUPPORT(B): epoll_create1
 * Current: creates an eventpoll file and supports EPOLL_CLOEXEC.
 * Unsupported errno: unknown flags return -EINVAL.
 * Future: add nested, close, and epoll-fd readiness coverage.
 */
ssize_t sys_epoll_create1(struct trap_frame *tf)
{
	int flags = (int)syscall_arg(tf, 0);
	struct file *file __cleanup_with(file) = NULL;
	int fd;

	if (!sys_epoll_create1_flags_ok(flags))
		return -EINVAL;

	file = eventpoll_file_alloc();
	if (!file)
		return -ENOMEM;

	fd = fd_alloc_flags(file, flags);
	if (fd < 0)
		return fd;

	cleanup_forget_ptr(file);
	return fd;
}

/*
 * SYSCALL_SUPPORT(B): epoll_ctl
 * Current: supports ADD, MOD, and DEL for poll-capable non-epoll fds.
 * Unsupported errno: invalid ops, targets, event bits, EPOLLET, and
 * EPOLLONESHOT return -EINVAL; non-pollable fds return -EPERM.
 * Future: implement edge/oneshot trigger strategies when needed.
 */
ssize_t sys_epoll_ctl(struct trap_frame *tf)
{
	int epfd = (int)syscall_arg(tf, 0);
	int op = (int)syscall_arg(tf, 1);
	int fd = (int)syscall_arg(tf, 2);
	const struct epoll_event *uevent = (const struct epoll_event *)syscall_arg(tf, 3);
	struct file *epfile __cleanup_with(file) = NULL;
	struct file *file __cleanup_with(file) = NULL;
	struct epoll_event event;
	const struct epoll_event *eventp = NULL;

	if (!sys_epoll_op_valid(op))
		return -EINVAL;

	epfile = fd_get(epfd);
	if (!epfile)
		return -EBADF;
	if (!eventpoll_file(epfile))
		return -EINVAL;
	if (fd == epfd)
		return -EINVAL;

	file = fd_get(fd);
	if (!file)
		return -EBADF;
	if (eventpoll_file(file))
		return -EINVAL;
	if (!file->f_op || !file->f_op->poll)
		return -EPERM;

	switch (op) {
	case EPOLL_CTL_ADD:
	case EPOLL_CTL_MOD:
		if (!uevent)
			return -EFAULT;
		if (copy_from_user(&event, uevent, sizeof(event)) != 0)
			return -EFAULT;
		eventp = &event;
		break;
	case EPOLL_CTL_DEL:
		break;
	}

	return eventpoll_ctl(epfile, op, fd, file, eventp);
}

/*
 * SYSCALL_SUPPORT(B): epoll_pwait
 * Current: waits on registered level-triggered items with optional sigmask.
 * Unsupported errno: bad sigset size or maxevents returns -EINVAL.
 * Future: add nested, close, and epoll-fd readiness coverage.
 */
ssize_t sys_epoll_pwait(struct trap_frame *tf)
{
	int epfd = (int)syscall_arg(tf, 0);
	struct epoll_event *uevents = (struct epoll_event *)syscall_arg(tf, 1);
	int maxevents = (int)syscall_arg(tf, 2);
	long timeout = (long)syscall_arg(tf, 3);
	const unsigned long *usigmask = (const unsigned long *)syscall_arg(tf, 4);
	size_t sigsetsize = (size_t)syscall_arg(tf, 5);
	struct epoll_event kevents[NR_OPEN];
	struct file *epfile __cleanup_with(file) = NULL;
	struct poll_sigmask_guard sigmask_guard __cleanup_with(
		poll_sigmask_restore) = {
		.task = current_task(),
		.old_blocked = 0,
		.active = false,
	};
	struct wait_deadline deadline;
	size_t scan_limit;
	int ret;

	if (maxevents <= 0)
		return -EINVAL;
	if (!uevents)
		return -EFAULT;
	if (!sys_epoll_wait_sigmask_ok(usigmask, sigsetsize))
		return -EINVAL;

	epfile = fd_get(epfd);
	if (!epfile)
		return -EBADF;
	if (!eventpoll_file(epfile))
		return -EINVAL;

	ret = mtime_deadline_from_ms(timeout, &deadline);
	if (ret < 0)
		return ret;

	ret = poll_apply_sigmask(usigmask, sigsetsize, &sigmask_guard);
	if (ret < 0)
		return ret;

	scan_limit = (size_t)maxevents;
	if (scan_limit > ARRLEN(kevents))
		scan_limit = ARRLEN(kevents);

	ret = eventpoll_wait(epfile, kevents, (int)scan_limit, &deadline);
	if (ret == -EINTR)
		poll_defer_sigmask_restore(&sigmask_guard);
	if (ret > 0 && copy_to_user(uevents, kevents,
				    (size_t)ret * sizeof(kevents[0])) != 0)
		return -EFAULT;

	return ret;
}

/*
 * SYSCALL_SUPPORT(B): ppoll
 * Current: scans pollfd entries with timeout and optional temporary sigmask.
 * Unsupported errno: nfds above NR_OPEN or invalid sigset size returns
 * -EINVAL; signal race semantics are simplified.
 * Future: document NR_OPEN limits and add signal interruption coverage.
 */
ssize_t sys_ppoll(struct trap_frame *tf)
{
	struct pollfd *ufds = (struct pollfd *)syscall_arg(tf, 0);
	size_t nfds = (size_t)syscall_arg(tf, 1);
	const struct timespec *utimeout = (const struct timespec *)syscall_arg(tf, 2);
	const unsigned long *usigmask = (const unsigned long *)syscall_arg(tf, 3);
	size_t sigsetsize = (size_t)syscall_arg(tf, 4);
	struct pollfd fds[NR_OPEN];
	struct file *files[NR_OPEN] = {0};
	struct timespec timeout;
	struct ppoll_scan_ctx scan_ctx;
	struct wait_request source;
	struct poll_sigmask_guard sigmask_guard __cleanup_with(
		poll_sigmask_restore) = {
		.task = current_task(),
		.old_blocked = 0,
		.active = false,
	};
	struct wait_deadline deadline;
	int ret;

	if (nfds > NR_OPEN)
		return -EINVAL;
	if (nfds > 0 && !ufds)
		return -EFAULT;
	if (usigmask && sigsetsize != sizeof(uint64_t))
		return -EINVAL;
	if (!usigmask && sigsetsize != 0 && sigsetsize != sizeof(uint64_t))
		return -EINVAL;

	if (utimeout) {
		if (copy_from_user(&timeout, utimeout, sizeof(timeout)) != 0)
			return -EFAULT;
		ret = mtime_deadline_from_timespec(&timeout, &deadline);
		if (ret < 0)
			return ret;
	} else {
		ret = mtime_deadline_from_timespec(NULL, &deadline);
		if (ret < 0)
			return ret;
	}

	if (nfds > 0 && copy_from_user(fds, ufds, nfds * sizeof(fds[0])) != 0)
		return -EFAULT;
	for (size_t i = 0; i < nfds; i++) {
		if (fds[i].fd >= 0)
			files[i] = fd_get(fds[i].fd);
	}

	ret = poll_apply_sigmask(usigmask, sigsetsize, &sigmask_guard);
	if (ret < 0) {
		poll_file_snapshot_put(files, nfds);
		return ret;
	}

	scan_ctx.fds = fds;
	scan_ctx.files = files;
	scan_ctx.nfds = nfds;
	scan_ctx.ready = 0;
	source.kind = WAIT_KIND_POLL;
	source.check = ppoll_scan;
	source.arg = &scan_ctx;
	source.channel_limit = WAIT_SESSION_MAX_CHANNELS;
	ret = poll_wait(&source, &deadline, &scan_ctx.ready);
	poll_file_snapshot_put(files, nfds);
	if (ret == -EINTR)
		poll_defer_sigmask_restore(&sigmask_guard);

	if (ret >= 0 && nfds > 0 &&
	    copy_to_user(ufds, fds, nfds * sizeof(fds[0])) != 0)
		return -EFAULT;

	return ret;
}

/*
 * SYSCALL_SUPPORT(B): pselect6
 * Current: scans fd_sets with timeout and optional packed sigmask.
 * Unsupported errno: nfds outside [0, NR_OPEN] or invalid sigmask metadata
 * returns -EINVAL; signal race semantics are simplified.
 * Future: add signal interruption and race-behavior coverage.
 */
ssize_t sys_pselect6(struct trap_frame *tf)
{
	long nfds = (long)syscall_arg(tf, 0);
	fd_set *ureadfds = (fd_set *)syscall_arg(tf, 1);
	fd_set *uwritefds = (fd_set *)syscall_arg(tf, 2);
	fd_set *uexceptfds = (fd_set *)syscall_arg(tf, 3);
	const struct timespec *utimeout = (const struct timespec *)syscall_arg(tf, 4);
	const struct pselect6_sigmask *usigpack =
		(const struct pselect6_sigmask *)syscall_arg(tf, 5);
	const unsigned long *usigmask;
	fd_set in_readfds;
	fd_set in_writefds;
	fd_set in_exceptfds;
	fd_set out_readfds;
	fd_set out_writefds;
	fd_set out_exceptfds;
	struct file *files[NR_OPEN] = {0};
	struct timespec timeout;
	struct pselect_scan_ctx scan_ctx;
	struct wait_request source;
	struct poll_sigmask_guard sigmask_guard
		__cleanup_with(poll_sigmask_restore) = {
		.task = current_task(),
		.old_blocked = 0,
		.active = false,
	};
	struct wait_deadline deadline;
	size_t sigsetsize;
	size_t fdset_bytes;
	int ready;
	int ret;

	if (nfds < 0 || nfds > NR_OPEN)
		return -EINVAL;

	if (utimeout) {
		if (copy_from_user(&timeout, utimeout, sizeof(timeout)) != 0)
			return -EFAULT;
		ret = mtime_deadline_from_timespec(&timeout, &deadline);
		if (ret < 0)
			return ret;
	} else {
		ret = mtime_deadline_from_timespec(NULL, &deadline);
		if (ret < 0)
			return ret;
	}

	fdset_bytes = sys_fdset_nbytes((size_t)nfds);
	ret = pselect_copy_fdset(&in_readfds, ureadfds, fdset_bytes);
	if (ret < 0)
		return ret;
	ret = pselect_copy_fdset(&in_writefds, uwritefds, fdset_bytes);
	if (ret < 0)
		return ret;
	ret = pselect_copy_fdset(&in_exceptfds, uexceptfds, fdset_bytes);
	if (ret < 0)
		return ret;

	memset(&out_readfds, 0, sizeof(out_readfds));
	memset(&out_writefds, 0, sizeof(out_writefds));
	memset(&out_exceptfds, 0, sizeof(out_exceptfds));
	for (size_t fd = 0; fd < (size_t)nfds; fd++) {
		if (sys_fdset_test(&in_readfds, (int)fd) ||
		    sys_fdset_test(&in_writefds, (int)fd) ||
		    sys_fdset_test(&in_exceptfds, (int)fd)) {
			files[fd] = fd_get((int)fd);
			if (!files[fd]) {
				poll_file_snapshot_put(files, (size_t)nfds);
				return -EBADF;
			}
		}
	}

	ret = pselect_copy_sigmask_args(usigpack, &usigmask, &sigsetsize);
	if (ret < 0) {
		poll_file_snapshot_put(files, (size_t)nfds);
		return ret;
	}
	ret = poll_apply_sigmask(usigmask, sigsetsize, &sigmask_guard);
	if (ret < 0) {
		poll_file_snapshot_put(files, (size_t)nfds);
		return ret;
	}

	scan_ctx.in_readfds = ureadfds ? &in_readfds : NULL;
	scan_ctx.in_writefds = uwritefds ? &in_writefds : NULL;
	scan_ctx.in_exceptfds = uexceptfds ? &in_exceptfds : NULL;
	scan_ctx.out_readfds = ureadfds ? &out_readfds : NULL;
	scan_ctx.out_writefds = uwritefds ? &out_writefds : NULL;
	scan_ctx.out_exceptfds = uexceptfds ? &out_exceptfds : NULL;
	scan_ctx.files = files;
	scan_ctx.nfds = (size_t)nfds;
	scan_ctx.ready = 0;
	source.kind = WAIT_KIND_POLL;
	source.check = pselect_scan;
	source.arg = &scan_ctx;
	source.channel_limit = WAIT_SESSION_MAX_CHANNELS;
	ready = poll_wait(&source, &deadline, &scan_ctx.ready);
	poll_file_snapshot_put(files, (size_t)nfds);
	if (ready == -EINTR)
		poll_defer_sigmask_restore(&sigmask_guard);
	if (ready < 0)
		return ready;

	ret = pselect_copy_result_fdset(ureadfds, &out_readfds, fdset_bytes);
	if (ret < 0)
		return ret;
	ret = pselect_copy_result_fdset(uwritefds, &out_writefds, fdset_bytes);
	if (ret < 0)
		return ret;
	ret = pselect_copy_result_fdset(uexceptfds, &out_exceptfds,
					fdset_bytes);
	if (ret < 0)
		return ret;

	return ready;
}

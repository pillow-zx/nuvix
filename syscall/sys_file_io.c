/*
 * syscall/sys_file_io.c - fd I/O 和 iovec 系统调用
 */

#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/fs_struct.h>
#include <nuvix/signal.h>
#include <nuvix/stat.h>
#include <nuvix/statfs.h>
#include <nuvix/types.h>
#include <nuvix/errno.h>
#include <nuvix/syscall.h>
#include <nuvix/mm.h>
#include <nuvix/buddy.h>
#include <nuvix/pipe.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/tools.h>
#include <nuvix/vfs.h>
#include <uapi/uio.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/trap.h>
#include <nuvix/time.h>

#include "sys_file_internal.h"

#define F_GETLK64		 12
#define F_SETLK64		 13
#define F_SETLKW64		 14
#define F_CREATED_QUERY		 1028
#define F_GETDELEG		 1039
#define F_SETDELEG		 1040
#define SPLICE_F_SUPPORTED_HINTS (SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_GIFT)

#define MAX_FILE_SIZE (1ULL << 40)

enum fcntl_cmd_status {
	FCNTL_CMD_SUPPORTED,
	FCNTL_CMD_UNSUPPORTED,
};

struct fcntl_cmd_support {
	int cmd;
	enum fcntl_cmd_status status;
	int unsupported_errno;
};

static const struct fcntl_cmd_support fcntl_cmds[] = {
	{F_DUPFD, FCNTL_CMD_SUPPORTED, 0},
	{F_GETFD, FCNTL_CMD_SUPPORTED, 0},
	{F_SETFD, FCNTL_CMD_SUPPORTED, 0},
	{F_GETFL, FCNTL_CMD_SUPPORTED, 0},
	{F_SETFL, FCNTL_CMD_SUPPORTED, 0},
	{F_GETLK, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETLK, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETLKW, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETOWN, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETOWN, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETSIG, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETSIG, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETLK64, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETLK64, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETLKW64, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETOWN_EX, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETOWN_EX, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETOWNER_UIDS, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_OFD_GETLK, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_OFD_SETLK, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_OFD_SETLKW, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETLEASE, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETLEASE, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_NOTIFY, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_DUPFD_QUERY, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_CREATED_QUERY, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_CANCELLK, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_DUPFD_CLOEXEC, FCNTL_CMD_SUPPORTED, 0},
	{F_SETPIPE_SZ, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETPIPE_SZ, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_ADD_SEALS, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GET_SEALS, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GET_RW_HINT, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SET_RW_HINT, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GET_FILE_RW_HINT, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SET_FILE_RW_HINT, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_GETDELEG, FCNTL_CMD_UNSUPPORTED, -EINVAL},
	{F_SETDELEG, FCNTL_CMD_UNSUPPORTED, -EINVAL},
};

static struct file *fd_get_readable(int fd)
{
	struct file *file = fd_get(fd);

	if (!file)
		return NULL;
	if (!(file->f_mode & FMODE_READ) || !file->f_op || !file->f_op->read) {
		file_put(file);
		return NULL;
	}

	return file;
}

static struct file *fd_get_writable(int fd)
{
	struct file *file = fd_get(fd);

	if (!file)
		return NULL;
	if (!(file->f_mode & FMODE_WRITE) || !file->f_op ||
	    !file->f_op->write) {
		file_put(file);
		return NULL;
	}

	return file;
}

static ssize_t read_user_buffer_pos(struct file *file, void *buf, size_t len,
				    loff_t *pos)
{
	char kbuf[SYS_FILE_BUF_SIZE];
	size_t done = 0;

	if (!access_ok(buf, len))
		return -EFAULT;

	while (done < len) {
		size_t chunk = len - done;
		ssize_t ret;

		if (chunk > SYS_FILE_BUF_SIZE)
			chunk = SYS_FILE_BUF_SIZE;

		ret = vfs_read_pos(file, kbuf, chunk, pos);
		if (ret > 0) {
			size_t left = copy_to_user((char *)buf + done, kbuf,
						   (size_t)ret);

			if (left != 0) {
				if (pos)
					*pos -= (loff_t)left;
				else
					vfs_rewind_pos(file, (loff_t)left);
				done += (size_t)ret - left;
				return done ? (ssize_t)done : -EFAULT;
			}
		}

		if (ret < 0)
			return ret;
		if (ret == 0)
			break;

		done += (size_t)ret;
		if ((size_t)ret < chunk)
			break;
	}

	return (ssize_t)done;
}

static ssize_t write_user_buffer_pos(struct file *file, const void *buf,
				     size_t len, loff_t *pos)
{
	char kbuf[SYS_FILE_BUF_SIZE];
	size_t done = 0;

	if (!access_ok(buf, len))
		return -EFAULT;
	/* Keep a small pipe write as one VFS request for PIPE_BUF semantics. */
	if (!pos && pipe_file(file) && len <= PIPE_BUF) {
		char *pipe_buf;
		ssize_t ret;

		if (len == 0)
			return 0;
		pipe_buf = get_free_page(0, ALLOC_NOWAIT);
		if (!pipe_buf)
			return -ENOMEM;
		if (copy_from_user(pipe_buf, buf, len) != 0) {
			free_page(pipe_buf, 0);
			return -EFAULT;
		}
		ret = vfs_write(file, pipe_buf, len);
		free_page(pipe_buf, 0);
		return ret;
	}
	if (!pos && pipe_file(file) && len > PIPE_BUF) {
		size_t pipe_len = PIPE_BUF + 1;
		char *pipe_buf = kmalloc(pipe_len, ALLOC_NOWAIT);
		ssize_t ret;

		if (!pipe_buf)
			return -ENOMEM;
		if (copy_from_user(pipe_buf, buf, pipe_len) != 0) {
			kfree(pipe_buf);
			return -EFAULT;
		}
		ret = vfs_write(file, pipe_buf, pipe_len);
		kfree(pipe_buf);
		if (ret <= 0 || (size_t)ret < pipe_len || pipe_len == len)
			return ret;
		done = (size_t)ret;
	}

	while (done < len) {
		size_t chunk = len - done;
		ssize_t ret;

		if (chunk > SYS_FILE_BUF_SIZE)
			chunk = SYS_FILE_BUF_SIZE;

		if (copy_from_user(kbuf, (const char *)buf + done, chunk) != 0)
			return done ? (ssize_t)done : -EFAULT;

		ret = vfs_write_pos(file, kbuf, chunk, pos);
		if (ret < 0)
			return done ? (ssize_t)done : ret;
		if (ret == 0)
			break;

		done += (size_t)ret;
		if ((size_t)ret < chunk)
			break;
	}

	return (ssize_t)done;
}

static const struct fcntl_cmd_support *fcntl_cmd_lookup(int cmd)
{
	for (size_t i = 0; i < ARRLEN(fcntl_cmds); i++) {
		if (fcntl_cmds[i].cmd == cmd)
			return &fcntl_cmds[i];
	}

	return NULL;
}

static bool splice_regular_file(struct file *file)
{
	return file && file->f_inode && S_ISREG(file->f_inode->i_mode);
}

static int copy_user_offset(loff_t *uoffset, loff_t *offset)
{
	if (!access_ok(uoffset, sizeof(*uoffset)))
		return -EFAULT;
	if (copy_from_user(offset, uoffset, sizeof(*offset)) != 0)
		return -EFAULT;
	if (*offset < 0)
		return -EINVAL;

	return 0;
}

static ssize_t write_pipe_iovec_prefix(struct file *file,
				       const struct iovec *uiov, size_t iovcnt,
				       size_t *request_len, size_t *total_len)
{
	char *buffer;
	struct iovec iov;
	size_t total = 0;
	size_t copied = 0;
	size_t request;
	bool atomic;

	for (size_t i = 0; i < iovcnt; i++) {
		if (copy_from_user(&iov, uiov + i, sizeof(iov)) != 0)
			return -EFAULT;
		if (iov.iov_len > UINT64_MAX - total)
			return -EINVAL;
		total += iov.iov_len;
	}
	*total_len = total;
	if (total == 0)
		return 0;

	atomic = total <= PIPE_BUF;
	request = atomic ? total : PIPE_BUF + 1;
	*request_len = request;
	buffer = atomic ? get_free_page(0, ALLOC_NOWAIT) :
		kmalloc(request, ALLOC_NOWAIT);
	if (!buffer)
		return -ENOMEM;
	for (size_t i = 0; i < iovcnt; i++) {
		size_t chunk;

		if (copy_from_user(&iov, uiov + i, sizeof(iov)) != 0 ||
		    !access_ok(iov.iov_base, iov.iov_len))
			goto fault;
		chunk = iov.iov_len;
		if (chunk > request - copied)
			chunk = request - copied;
		if (copy_from_user(buffer + copied, iov.iov_base, chunk) != 0)
			goto fault;
		copied += chunk;
		if (copied == request)
			break;
	}

	ssize_t ret = vfs_write(file, buffer, request);

	if (atomic)
		free_page(buffer, 0);
	else
		kfree(buffer);
	return ret;

fault:
	if (atomic)
		free_page(buffer, 0);
	else
		kfree(buffer);
	return -EFAULT;
}

static ssize_t rw_iovec(struct file *file, const struct iovec *uiov,
			size_t iovcnt, bool write)
{
	struct iovec iov;
	size_t skipped = 0;
	ssize_t total = 0;

	if (iovcnt > SYS_IOV_MAX)
		return -EINVAL;
	if (write && pipe_file(file)) {
		size_t request_len = 0;
		size_t total_len = 0;
		ssize_t ret = write_pipe_iovec_prefix(file, uiov, iovcnt,
						      &request_len, &total_len);

		if (ret <= 0 || (size_t)ret < request_len ||
		    (size_t)ret == total_len)
			return ret;
		total = ret;
		skipped = (size_t)ret;
	}

	for (size_t i = 0; i < iovcnt; i++) {
		uintptr_t base;
		size_t length;
		size_t done = 0;

		if (copy_from_user(&iov, uiov + i, sizeof(iov)) != 0)
			return total ? total : -EFAULT;
		if (skipped >= iov.iov_len) {
			skipped -= iov.iov_len;
			continue;
		}
		base = (uintptr_t)iov.iov_base + skipped;
		length = iov.iov_len - skipped;
		skipped = 0;
		if (!access_ok((void *)base, length))
			return total ? total : -EFAULT;

		while (done < length) {
			uintptr_t chunk_base = base + done;
			size_t chunk_len = length - done;
			ssize_t ret;

			if (write)
				ret = write_user_buffer_pos(
					file, (const void *)chunk_base,
					chunk_len, NULL);
			else
				ret = read_user_buffer_pos(file,
							   (void *)chunk_base,
							   chunk_len, NULL);

			if (ret < 0)
				return total ? total : ret;
			if (ret == 0)
				return total;

			done += (size_t)ret;
			total += ret;
		}
	}

	return total;
}

/*
 * SYSCALL_SUPPORT(A): write
 * Current: VFS write keeps pipe requests of at most PIPE_BUF intact, so the
 * pipe owns their all-or-nothing and O_NONBLOCK semantics. Linux riscv64
 * SA_RESTART replay applies after an interruptible wait returns -EINTR.
 * Future: keep partial-write and SIGPIPE restart boundaries under pipe tests.
 */
ssize_t sys_write(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	const char *buf = (const char *)syscall_arg(tf, 1);
	size_t len = syscall_arg(tf, 2);
	struct file *file __cleanup_with(file) = fd_get_writable(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;

	ret = write_user_buffer_pos(file, buf, len, NULL);
	return ret;
}

/*
 * SYSCALL_SUPPORT(A): read
 * Current: VFS read with Linux riscv64 SA_RESTART replay after an
 * interruptible wait returns -EINTR; a positive partial count is preserved.
 * Future: keep partial-read restart boundaries under pipe tests.
 */
ssize_t sys_read(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	char *buf = (char *)syscall_arg(tf, 1);
	size_t len = syscall_arg(tf, 2);
	struct file *file __cleanup_with(file) = fd_get_readable(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;

	ret = read_user_buffer_pos(file, buf, len, NULL);
	return ret;
}

ssize_t sys_readv(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	const struct iovec *uiov = (const struct iovec *)syscall_arg(tf, 1);
	size_t iovcnt = syscall_arg(tf, 2);
	struct file *file __cleanup_with(file) = fd_get_readable(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;
	if (!access_ok(uiov, iovcnt * sizeof(*uiov)))
		return -EFAULT;

	ret = rw_iovec(file, uiov, iovcnt, false);
	return ret;
}

/*
 * SYSCALL_SUPPORT(A): writev
 * Current: a pipe vector whose total length is at most PIPE_BUF is gathered
 * into one VFS write, preserving the pipe's atomic all-or-nothing contract.
 * Larger vectors retain the existing partial-I/O behavior.
 */
ssize_t sys_writev(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	const struct iovec *uiov = (const struct iovec *)syscall_arg(tf, 1);
	size_t iovcnt = syscall_arg(tf, 2);
	struct file *file __cleanup_with(file) = fd_get_writable(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;
	if (!access_ok(uiov, iovcnt * sizeof(*uiov)))
		return -EFAULT;

	ret = rw_iovec(file, uiov, iovcnt, true);
	return ret;
}

ssize_t sys_pread64(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	char *buf = (char *)syscall_arg(tf, 1);
	size_t len = syscall_arg(tf, 2);
	loff_t offset = (loff_t)syscall_arg(tf, 3);
	struct file *file __cleanup_with(file) = fd_get_readable(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;
	if (offset < 0)
		return -EINVAL;

	ret = read_user_buffer_pos(file, buf, len, &offset);
	return ret;
}

ssize_t sys_pwrite64(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	const char *buf = (const char *)syscall_arg(tf, 1);
	size_t len = syscall_arg(tf, 2);
	loff_t offset = (loff_t)syscall_arg(tf, 3);
	struct file *file __cleanup_with(file) = fd_get_writable(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;
	if (offset < 0)
		return -EINVAL;

	ret = write_user_buffer_pos(file, buf, len, &offset);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): sendfile
 * Current: buffered copy from a regular readable input file to writable output.
 * Unsupported errno: bad fds return -EBADF; non-regular input or O_APPEND
 * output returns -EINVAL.
 * Future: document the file-only contract before adding socket or pipe cases.
 */
ssize_t sys_sendfile(struct trap_frame *tf)
{
	int out_fd = (int)syscall_arg(tf, 0);
	int in_fd = (int)syscall_arg(tf, 1);
	loff_t *uoffset = (loff_t *)syscall_arg(tf, 2);
	size_t count = syscall_arg(tf, 3);
	struct file *out_file __cleanup_with(file) = fd_get_writable(out_fd);
	struct file *in_file __cleanup_with(file) = fd_get_readable(in_fd);
	loff_t offset;
	ssize_t ret;

	if (!out_file || !in_file)
		return -EBADF;
	if (!in_file->f_inode || (in_file->f_inode->i_mode & S_IFMT) != S_IFREG)
		return -EINVAL;
	if (out_file->f_flags & O_APPEND)
		return -EINVAL;
	if (count == 0)
		return 0;

	if (uoffset) {
		ret = copy_user_offset(uoffset, &offset);
		if (ret < 0)
			return ret;
		ret = vfs_copy_file_buffered(out_file, in_file, &offset, NULL,
					     count);
		if (ret > 0 &&
		    copy_to_user(uoffset, &offset, sizeof(offset)) != 0)
			return -EFAULT;
		return ret;
	}

	ret = vfs_copy_file_buffered(out_file, in_file, NULL, NULL, count);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): splice
 * Current: copies between one pipe endpoint and one regular file endpoint.
 * Unsupported errno: unknown flags, pipe-pipe, file-file, non-regular files,
 * and O_APPEND output return -EINVAL; pipe offsets return -ESPIPE.
 * Future: build an explicit pipe/file mode and flag support table.
 */
ssize_t sys_splice(struct trap_frame *tf)
{
	int fd_in = (int)syscall_arg(tf, 0);
	loff_t *uoff_in = (loff_t *)syscall_arg(tf, 1);
	int fd_out = (int)syscall_arg(tf, 2);
	loff_t *uoff_out = (loff_t *)syscall_arg(tf, 3);
	size_t len = syscall_arg(tf, 4);
	unsigned int flags = (unsigned int)syscall_arg(tf, 5);
	struct file *in_file __cleanup_with(file) = fd_get_readable(fd_in);
	struct file *out_file __cleanup_with(file) = fd_get_writable(fd_out);
	bool in_pipe;
	bool out_pipe;
	loff_t in_offset;
	loff_t out_offset;
	loff_t *in_offsetp = NULL;
	loff_t *out_offsetp = NULL;
	ssize_t ret;

	if (!in_file || !out_file)
		return -EBADF;
	if (flags & ~SPLICE_F_SUPPORTED_HINTS)
		return -EINVAL;
	if (len == 0)
		return 0;

	in_pipe = pipe_file(in_file);
	out_pipe = pipe_file(out_file);
	if (in_pipe == out_pipe)
		return -EINVAL;
	if (in_pipe && uoff_in)
		return -ESPIPE;
	if (out_pipe && uoff_out)
		return -ESPIPE;
	if (!in_pipe && !splice_regular_file(in_file))
		return -EINVAL;
	if (!out_pipe && !splice_regular_file(out_file))
		return -EINVAL;
	if (!out_pipe && (out_file->f_flags & O_APPEND))
		return -EINVAL;

	if (uoff_in) {
		ret = copy_user_offset(uoff_in, &in_offset);
		if (ret < 0)
			return ret;
		in_offsetp = &in_offset;
	}
	if (uoff_out) {
		ret = copy_user_offset(uoff_out, &out_offset);
		if (ret < 0)
			return ret;
		out_offsetp = &out_offset;
	}

	if (in_pipe)
		ret = pipe_splice_to_file(in_file, out_file, out_offsetp, len);
	else
		ret = vfs_copy_file_buffered(out_file, in_file, in_offsetp,
					     out_offsetp, len);
	if (ret > 0 && uoff_in &&
	    copy_to_user(uoff_in, &in_offset, sizeof(in_offset)) != 0)
		return -EFAULT;
	if (ret > 0 && uoff_out &&
	    copy_to_user(uoff_out, &out_offset, sizeof(out_offset)) != 0)
		return -EFAULT;
	return ret;
}

/*
 * SYSCALL_SUPPORT(A): close
 * Current: atomically detaches the fd slot and its close-on-exec bit before
 * dropping the referenced file outside the fdtable lock.
 */
ssize_t sys_close(struct trap_frame *tf)
{
	return fd_close((int)syscall_arg(tf, 0));
}

ssize_t sys_lseek(struct trap_frame *tf)
{
	struct file *file __cleanup_with(file) =
		fd_get((int)syscall_arg(tf, 0));
	loff_t offset = (loff_t)syscall_arg(tf, 1);
	int whence = (int)syscall_arg(tf, 2);
	ssize_t ret;

	if (!file)
		return -EBADF;

	ret = vfs_llseek(file, offset, whence);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): ioctl
 * Current: delegates to VFS; console handles termios and winsize commands.
 * Unsupported errno: bad fd returns -EBADF; no handler or unknown device
 * command returns -ENOTTY through VFS/device fops.
 * Future: extend and document tty/console probing commands.
 */
ssize_t sys_ioctl(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	uint64_t cmd = syscall_arg(tf, 1);
	uint64_t arg = syscall_arg(tf, 2);
	struct file *file __cleanup_with(file) = fd_get(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;

	ret = vfs_ioctl(file, cmd, arg);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): fcntl
 * Current: supports dup, descriptor-local close-on-exec, and file status flag
 * get/set commands.
 * Unsupported errno: bad fds return -EBADF first. Known but unsupported lock,
 * owner, lease, pipe-size, seal, rw-hint, notify, and delegation commands
 * return -EINVAL. Unknown commands also return -EINVAL for valid fds.
 * Future: replace individual table entries as each command grows real
 * subsystem semantics.
 */
ssize_t sys_fcntl(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	int cmd = (int)syscall_arg(tf, 1);
	unsigned long arg = syscall_arg(tf, 2);
	const struct fcntl_cmd_support *support;
	int ret;

	support = fcntl_cmd_lookup(cmd);
	if (!support) {
		struct file *file __cleanup_with(file) = fd_get(fd);

		if (!file)
			return -EBADF;
		return -EINVAL;
	}
	if (support->status == FCNTL_CMD_UNSUPPORTED) {
		struct file *file __cleanup_with(file) = fd_get(fd);

		if (!file)
			return -EBADF;
		return support->unsupported_errno;
	}

	switch (cmd) {
	case F_DUPFD:
		return fd_dup_from(fd, arg, 0);
	case F_GETFD:
		ret = fd_get_close_on_exec(fd);
		if (ret < 0)
			return ret;
		return ret ? FD_CLOEXEC : 0;
	case F_SETFD:
		return fd_set_close_on_exec(fd, arg & FD_CLOEXEC);
	case F_GETFL: {
		struct file *file __cleanup_with(file) = fd_get(fd);

		if (!file)
			return -EBADF;
		return file_get_status_flags(file);
	}
	case F_SETFL: {
		struct file *file __cleanup_with(file) = fd_get(fd);

		if (!file)
			return -EBADF;
		ret = file_set_status_flags(file, (uint32_t)arg);
		return ret;
	}
	case F_DUPFD_CLOEXEC:
		return fd_dup_from(fd, arg, 1);
	default:
		return -EINVAL;
	}
}

/*
 * SYSCALL_SUPPORT(A): dup and dup3
 * Current: duplicate fd slots share one open file description. dup clears the
 * new slot's close-on-exec bit; dup3 atomically replaces its target and may
 * set that bit with O_CLOEXEC. dup3 rejects equal descriptors with -EINVAL.
 */
ssize_t sys_dup(struct trap_frame *tf)
{
	return fd_dup((int)syscall_arg(tf, 0));
}

ssize_t sys_dup3(struct trap_frame *tf)
{
	int oldfd = (int)syscall_arg(tf, 0);
	int newfd = (int)syscall_arg(tf, 1);
	int flags = (int)syscall_arg(tf, 2);

	if (oldfd == newfd)
		return -EINVAL;
	if (flags & ~O_CLOEXEC)
		return -EINVAL;

	return fd_dup2(oldfd, newfd, flags & O_CLOEXEC);
}

ssize_t sys_fsync(struct trap_frame *tf)
{
	struct file *file __cleanup_with(file) =
		fd_get((int)syscall_arg(tf, 0));
	ssize_t ret;

	if (!file)
		return -EBADF;

	ret = vfs_sync_file(file);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): fdatasync
 * Current: flushes dirty file data through VFS, then asks the filesystem to
 * sync metadata needed to retrieve that data; filesystems without a datasync
 * hook fall back to full inode metadata writeback.
 * Unsupported errno: fd errors match fsync; storage ordering is best-effort.
 * Future: deepen per-filesystem ordering semantics if crash consistency
 * becomes a project goal.
 */
ssize_t sys_fdatasync(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	struct file *file __cleanup_with(file) = fd_get(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;

	ret = vfs_datasync_file(file);
	return ret;
}

ssize_t sys_ftruncate64(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	int64_t length = (int64_t)syscall_arg(tf, 1);
	struct file *file __cleanup_with(file) = fd_get(fd);
	ssize_t ret;

	if (!file)
		return -EBADF;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (length < 0 || length > (int64_t)MAX_FILE_SIZE)
		return -EINVAL;
	if (!file->f_inode)
		return -EINVAL;

	ret = vfs_truncate_file(file, (uint64_t)length);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): fallocate
 * Current: supports mode 0 allocation within the current maximum file size.
 * Unsupported errno: nonzero mode and invalid ranges return -EINVAL; too-large
 * ranges return -EFBIG.
 * Future: fix a flag/mode table before adding punch-hole or keep-size support.
 */
ssize_t sys_fallocate(struct trap_frame *tf)
{
	struct file *file __cleanup_with(file) =
		fd_get((int)syscall_arg(tf, 0));
	int mode = (int)syscall_arg(tf, 1);
	int64_t offset = (int64_t)syscall_arg(tf, 2);
	int64_t len = (int64_t)syscall_arg(tf, 3);
	uint64_t uoffset;
	uint64_t ulen;
	ssize_t ret;

	if (!file)
		return -EBADF;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (mode != 0)
		return -EINVAL;
	if (offset < 0 || len <= 0)
		return -EINVAL;

	uoffset = (uint64_t)offset;
	ulen = (uint64_t)len;
	if (uoffset > MAX_FILE_SIZE || ulen > MAX_FILE_SIZE - uoffset)
		return -EFBIG;

	ret = vfs_fallocate_file(file, mode, uoffset, ulen);
	return ret;
}

/*
 * SYSCALL_SUPPORT(A): pipe2
 * Current: creates separate read/write open file descriptions, applies
 * O_NONBLOCK to both status flags and O_CLOEXEC to both descriptor slots.
 * Failed fd installation or userspace result copying rolls both endpoints back.
 */
ssize_t sys_pipe2(struct trap_frame *tf)
{
	int *user_fds = (int *)syscall_arg(tf, 0);
	int flags = (int)syscall_arg(tf, 1);
	int fds[2];
	int ret;

	if (flags & ~(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;
	if (!access_ok(user_fds, sizeof(int[2])))
		return -EFAULT;

	ret = do_pipe2(fds, flags);
	if (ret < 0)
		return ret;

	if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
		fd_close(fds[0]);
		fd_close(fds[1]);
		return -EFAULT;
	}

	return 0;
}

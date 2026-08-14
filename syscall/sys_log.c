/*
 * syscall/sys_log.c - syslog ABI compatibility wrapper
 */

#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <uapi/syslog.h>
#include <nuvix/trap.h>

__must_check __pure
static inline bool syslog_action_valid(const int type)
{
	return type >= SYSLOG_ACTION_CLOSE && type <= SYSLOG_ACTION_SIZE_BUFFER;
}

__must_check __pure
static inline bool syslog_action_requires_root(const int type)
{
	switch (type) {
	case SYSLOG_ACTION_READ_ALL:
	case SYSLOG_ACTION_SIZE_BUFFER:
		return false;
	default:
		return true;
	}
}

/*
 * SYSCALL_SUPPORT(B): syslog
 * Current: a bounded printk byte ring supports destructive read, snapshots,
 * clear markers, unread size, and total capacity. UID 0 stands in for Linux
 * CAP_SYSLOG; READ_ALL and SIZE_BUFFER remain readable by every task.
 * Unsupported errno: authorized console controls return -ENOSYS; invalid
 * actions and read buffers return -EINVAL; inaccessible buffers return
 * -EFAULT.
 * Future: add a credential capability model and console loglevel policy.
 */
ssize_t sys_syslog(struct trap_frame *tf)
{
	int type = (int)syscall_arg(tf, 0);
	void *buffer = (void *)syscall_arg(tf, 1);
	int size = (int)syscall_arg(tf, 2);

	if (!syslog_action_valid(type))
		return -EINVAL;
	if ((type == SYSLOG_ACTION_READ || type == SYSLOG_ACTION_READ_ALL ||
	     type == SYSLOG_ACTION_READ_CLEAR) &&
	    (!buffer || size < 0))
		return -EINVAL;
	if (syslog_action_requires_root(type) && task_uid(current_task()) != 0)
		return -EPERM;

	switch (type) {
	case SYSLOG_ACTION_CLOSE:
	case SYSLOG_ACTION_OPEN:
		return 0;
	case SYSLOG_ACTION_READ:
		return printk_log_read(buffer, (size_t)size);
	case SYSLOG_ACTION_READ_ALL:
		return printk_log_read_all(buffer, (size_t)size, false);
	case SYSLOG_ACTION_READ_CLEAR:
		return printk_log_read_all(buffer, (size_t)size, true);
	case SYSLOG_ACTION_CLEAR:
		printk_log_clear();
		return 0;
	case SYSLOG_ACTION_SIZE_UNREAD:
		return (ssize_t)printk_log_unread_size();
	case SYSLOG_ACTION_SIZE_BUFFER:
		return (ssize_t)printk_log_buffer_size();
	default:
		return -ENOSYS;
	}
}

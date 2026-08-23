/*
 * syscall/sys_misc.c - 轻量兼容系统调用
 */

#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/futex.h>
#include <nuvix/fs.h>
#include <nuvix/fs_struct.h>
#include <nuvix/mm.h>
#include <nuvix/pid.h>
#include <nuvix/proc.h>
#include <nuvix/resource.h>
#include <nuvix/random.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/signal.h>
#include <nuvix/timer.h>
#include <uapi/random.h>
#include <uapi/sysinfo.h>
#include <uapi/utsname.h>
#include <nuvix/printk.h>
#include <nuvix/reboot.h>
#include <nuvix/trap.h>
#include <nuvix/vfs.h>
#include <uapi/reboot.h>

#define GRND_VALID_FLAGS (GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)

void rlimits_init(struct rlimit64 rlimits[RLIM_NLIMITS])
{
	if (!rlimits)
		return;

	for (int i = 0; i < RLIM_NLIMITS; i++) {
		rlimits[i].rlim_cur = RLIM_INFINITY;
		rlimits[i].rlim_max = RLIM_INFINITY;
	}
	rlimits[RLIMIT_NOFILE].rlim_cur = NR_OPEN;
	rlimits[RLIMIT_NOFILE].rlim_max = NR_OPEN;
}

static void uts_copy(char dst[UTS_FIELD_LEN], const char *src)
{
	size_t len = strnlen(src, UTS_FIELD_LEN - 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/*
 * SYSCALL_SUPPORT(B): sync
 * Current: synchronously writes all dirty page-cache data and reports success,
 * matching Linux sync(2)'s lack of an error result.
 * Unsupported: no per-superblock writeback accounting or error reporting.
 * Future: preserve the syscall contract as writeback grows more asynchronous.
 */
ssize_t sys_sync(struct trap_frame *tf)
{
	int ret;

	(void)tf;
	ret = vfs_sync_all();
	if (ret < 0)
		pr_warn("sync: VFS writeback failed (%d)\n", ret);
	return 0;
}

static bool reboot_magic2_valid(unsigned int magic)
{
	return magic == LINUX_REBOOT_MAGIC2 || magic == LINUX_REBOOT_MAGIC2A ||
	       magic == LINUX_REBOOT_MAGIC2B || magic == LINUX_REBOOT_MAGIC2C;
}

/*
 * SYSCALL_SUPPORT(B): reboot
 * Current: validates Linux magic values, maps commands to the kernel reboot
 * service, and leaves authorization and platform dispatch to that service.
 * Unsupported errno: other commands return -EINVAL.
 * Future: add restart2 only when boot-command storage exists.
 */
ssize_t sys_reboot(struct trap_frame *tf)
{
	unsigned int magic1 = (unsigned int)syscall_arg(tf, 0);
	unsigned int magic2 = (unsigned int)syscall_arg(tf, 1);
	unsigned int command = (unsigned int)syscall_arg(tf, 2);

	if (magic1 != LINUX_REBOOT_MAGIC1 || !reboot_magic2_valid(magic2))
		return -EINVAL;

	switch (command) {
	case LINUX_REBOOT_CMD_CAD_OFF:
		return kernel_reboot(KERNEL_REBOOT_CAD_OFF);
	case LINUX_REBOOT_CMD_CAD_ON:
		return kernel_reboot(KERNEL_REBOOT_CAD_ON);
	case LINUX_REBOOT_CMD_RESTART:
		return kernel_reboot(KERNEL_REBOOT_RESTART);
	case LINUX_REBOOT_CMD_HALT:
		return kernel_reboot(KERNEL_REBOOT_HALT);
	case LINUX_REBOOT_CMD_POWER_OFF:
		return kernel_reboot(KERNEL_REBOOT_POWER_OFF);
	default:
		return -EINVAL;
	}
}

ssize_t sys_uname(struct trap_frame *tf)
{
	struct utsname *u = (struct utsname *)syscall_arg(tf, 0);
	struct utsname k;

	if (!u)
		return -EFAULT;

	memset(&k, 0, sizeof(k));
	uts_copy(k.sysname, "nuvix");
	uts_copy(k.nodename, "nuvix");
	uts_copy(k.release, "0.0.6");
	uts_copy(k.version, "nuvix teaching kernel");
	uts_copy(k.machine, "riscv64");
	uts_copy(k.domainname, "(none)");

	if (copy_to_user(u, &k, sizeof(k)) != 0)
		return -EFAULT;

	return 0;
}

ssize_t sys_set_tid_addr(struct trap_frame *tf)
{
	task_set_clear_child_tid(current_task(), (int *)syscall_arg(tf, 0));
	return current_task()->tid ? (ssize_t)current_task()->tid->nr : 0;
}

/*
 * SYSCALL_SUPPORT(B): setuid
 * Current: root may set any uid; non-root may only set its current uid.
 * Unsupported errno: non-root attempts to change to a different uid return
 * -EPERM.
 * Future: replace this with saved/effective uid and capability semantics.
 */
ssize_t sys_setuid(struct trap_frame *tf)
{
	uint32_t uid = (uint32_t)syscall_arg(tf, 0);

	if (task_uid(current_task()) != 0 && task_uid(current_task()) != uid)
		return -EPERM;

	return task_set_uid(current_task(), uid);
}

/*
 * SYSCALL_SUPPORT(B): setgid
 * Current: root may set any gid; non-root may only set its current gid.
 * Unsupported errno: non-root attempts to change to a different gid return
 * -EPERM.
 * Future: replace this with saved/effective gid and capability semantics.
 */
ssize_t sys_setgid(struct trap_frame *tf)
{
	uint32_t gid = (uint32_t)syscall_arg(tf, 0);

	if (task_gid(current_task()) != 0 && task_gid(current_task()) != gid)
		return -EPERM;

	return task_set_gid(current_task(), gid);
}

/*
 * SYSCALL_SUPPORT(B): getgroups
 * Current: returns the per-task supplementary-group list (ngroups capped at
 * NGROUPS_MAX); size 0 queries the count without copying; a too-small buffer
 * returns -EINVAL; invalid user pointers return -EFAULT.
 * Unsupported errno: none beyond the Linux contract.
 * Future: keep the ABI when a capability subsystem replaces the euid-0 gate.
 */
ssize_t sys_getgroups(struct trap_frame *tf)
{
	int size = (int)syscall_arg(tf, 0);
	gid_t *groups = (gid_t *)syscall_arg(tf, 1);
	struct cred *cred = current_task()->cred;
	uint32_t ngroups = cred ? cred->ngroups : 0;

	if (size < 0)
		return -EINVAL;
	if (size == 0)
		return (ssize_t)ngroups;
	if (size < (int)ngroups)
		return -EINVAL;
	if (ngroups > 0 && (!groups ||
			    copy_to_user(groups, cred->groups,
					 ngroups * sizeof(gid_t))))
		return -EFAULT;

	return (ssize_t)ngroups;
}

/*
 * SYSCALL_SUPPORT(B): setgroups
 * Current: euid 0 may set up to NGROUPS_MAX supplementary groups; size 0
 * clears the list; non-root callers get -EPERM; oversized sizes return
 * -EINVAL; invalid user pointers return -EFAULT.
 * Unsupported errno: none beyond the Linux contract.
 * Future: replace the euid-0 gate with CAP_SETGID when capabilities land.
 */
ssize_t sys_setgroups(struct trap_frame *tf)
{
	int size = (int)syscall_arg(tf, 0);
	gid_t *groups = (gid_t *)syscall_arg(tf, 1);
	gid_t kgroups[NGROUPS_MAX];

	if (size < 0)
		return -EINVAL;
	if (size > NGROUPS_MAX)
		return -EINVAL;
	if (task_euid(current_task()) != 0)
		return -EPERM;
	if (size > 0) {
		if (!groups ||
		    copy_from_user(kgroups, groups, size * sizeof(gid_t)))
			return -EFAULT;
	}

	return task_set_groups(current_task(), kgroups, (uint32_t)size);
}

ssize_t sys_umask(struct trap_frame *tf)
{
	uint32_t mask = (uint32_t)syscall_arg(tf, 0) & 0777;

	return fs_set_umask(
		current_task()->proc ? current_task()->proc->fs : NULL, mask);
}

/*
 * SYSCALL_SUPPORT(B): sysinfo
 * Current: reports uptime, total/free RAM, process count, and mem_unit.
 * Unsupported errno: unsupported load, swap, and high-memory fields are zeroed
 * rather than rejected.
 * Future: document zeroed fields or populate them from future accounting.
 */
ssize_t sys_sysinfo(struct trap_frame *tf)
{
	struct sysinfo *uinfo = (struct sysinfo *)syscall_arg(tf, 0);
	struct sysinfo info;

	if (!uinfo)
		return -EFAULT;

	memset(&info, 0, sizeof(info));
	info.uptime = (int64_t)(timer_now() / MTIME_FREQ);
	info.totalram = DRAM_SIZE;
	info.freeram = buddy_free_pages() * PAGE_SIZE;
	info.procs = pid_count_tasks();
	info.mem_unit = 1;
	if (copy_to_user(uinfo, &info, sizeof(info)) != 0)
		return -EFAULT;

	return 0;
}

/*
 * SYSCALL_SUPPORT(B): prlimit64
 * Current: gets/sets per-signal rlimits for self or same-thread-group leader.
 * Unsupported errno: invalid resource returns -EINVAL; cross-task access
 * outside the current thread group returns -EPERM.
 * Future: enforce more resources, starting with NOFILE and AS.
 */
ssize_t sys_prlimit64(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	int resource = (int)syscall_arg(tf, 1);
	const struct rlimit64 *unew =
		(const struct rlimit64 *)syscall_arg(tf, 2);
	struct rlimit64 *uold = (struct rlimit64 *)syscall_arg(tf, 3);
	struct proc_struct *proc;
	struct rlimit64 new_limit;
	bool put_proc = false;
	ssize_t ret = 0;

	if (resource < 0 || resource >= RLIM_NLIMITS)
		return -EINVAL;
	if (pid < 0)
		return -ESRCH;

	if (pid == 0) {
		proc = current_task()->proc;
	} else {
		struct task_struct *leader;

		proc = pid_lookup_proc((pid_t)pid);
		put_proc = true;
		if (!proc)
			return -ESRCH;
		leader = proc_leader_get(proc);
		if (!leader) {
			ret = -ESRCH;
			goto out;
		}
		task_put(leader);
		if (!current_task() || current_task()->proc != proc) {
			ret = -EPERM;
			goto out;
		}
	}

	if (!proc) {
		ret = -ESRCH;
		goto out;
	}

	if (unew) {
		if (copy_from_user(&new_limit, unew, sizeof(new_limit)) != 0) {
			ret = -EFAULT;
			goto out;
		}
		if (new_limit.rlim_cur > new_limit.rlim_max) {
			ret = -EINVAL;
			goto out;
		}
	}

	spin_lock(&proc->lock);
	if (uold) {
		struct rlimit64 old = proc->rlimits[resource];

		spin_unlock(&proc->lock);
		if (copy_to_user(uold, &old, sizeof(old)) != 0) {
			ret = -EFAULT;
			goto out;
		}
		spin_lock(&proc->lock);
	}
	if (unew)
		proc->rlimits[resource] = new_limit;
	spin_unlock(&proc->lock);

out:
	if (put_proc)
		proc_put(proc);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): getrusage
 * Current: reports basic self or children CPU time with many fields zeroed.
 * Unsupported errno: unsupported who values return -EINVAL.
 * Future: document or populate memory and I/O accounting fields.
 */
ssize_t sys_getrusage(struct trap_frame *tf)
{
	int who = (int)syscall_arg(tf, 0);
	struct rusage *uusage = (struct rusage *)syscall_arg(tf, 1);
	struct proc_cputime_snapshot snapshot;
	struct task_cputime time;
	struct rusage usage;

	if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)
		return -EINVAL;
	if (!uusage)
		return -EFAULT;

	if (who == RUSAGE_SELF) {
		time = (struct task_cputime){
			.utime_ticks = task_user_ticks(current_task()),
			.stime_ticks = task_system_ticks(current_task()),
		};
	} else {
		proc_cputime_snapshot(current_task()->proc, &snapshot);
		time = snapshot.children;
	}
	cputime_rusage(&time, &usage);
	if (copy_to_user(uusage, &usage, sizeof(usage)) != 0)
		return -EFAULT;

	return 0;
}

/*
 * SYSCALL_SUPPORT(C): getrandom
 * Current: returns bytes from a weak xorshift/mtime-seeded generator.
 * Unsupported errno: unknown flags return -EINVAL; NULL output with nonzero
 * count returns -EFAULT.
 * Future: mark this weak random source or connect a real entropy source.
 */
ssize_t sys_getrandom(struct trap_frame *tf)
{
	uint8_t *ubuf = (uint8_t *)syscall_arg(tf, 0);
	size_t count = (size_t)syscall_arg(tf, 1);
	uint32_t flags = (uint32_t)syscall_arg(tf, 2);
	uint8_t chunk[64];
	size_t done = 0;

	if (flags & ~GRND_VALID_FLAGS)
		return -EINVAL;
	if (count == 0)
		return 0;
	if (!ubuf)
		return -EFAULT;

	while (done < count) {
		size_t n = count - done;

		if (n > sizeof(chunk))
			n = sizeof(chunk);
		weak_random_bytes(chunk, n);
		size_t left = copy_to_user(ubuf + done, chunk, n);
		size_t copied = n - left;

		done += copied;
		if (left != 0)
			return done ? (ssize_t)done : -EFAULT;
	}

	return (ssize_t)done;
}

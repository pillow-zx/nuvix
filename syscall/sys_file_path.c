/*
 * syscall/sys_file_path.c - 路径与目录操作系统调用
 */

#include <kernel/fdtable.h>
#include <kernel/fs.h>
#include <kernel/fs_struct.h>
#include <kernel/signal.h>
#include <kernel/stat.h>
#include <kernel/statfs.h>
#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/syscall.h>
#include <kernel/mm.h>
#include <kernel/buddy.h>
#include <kernel/pipe.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/page.h>
#include <kernel/trap.h>
#include <kernel/time.h>

#include "sys_file_internal.h"

struct getdents_ctx {
	char *dirp;
	size_t count;
	size_t written;
};

static uint32_t apply_umask(uint32_t mode)
{
	if (!current_task())
		return mode;

	return mode & ~fs_get_umask(current_task()->proc ?
						 current_task()->proc->fs : NULL);
}

static uint8_t vfs_type_to_dirent(uint8_t type)
{
	switch (type) {
	case 1:
		return DT_REG;
	case 2:
		return DT_DIR;
	case 3:
		return DT_CHR;
	case 4:
		return DT_BLK;
	case 5:
		return DT_FIFO;
	case 6:
		return DT_SOCK;
	case 7:
		return DT_LNK;
	default:
		return DT_UNKNOWN;
	}
}

static int sys_faccessat_path(int dfd, const char *upath, int mode, int flags,
			      uint32_t lookup_flags)
{
	struct vfs_at_lookup_result lookup __cleanup_with(vfs_at_lookup) = {};
	char *path __cleanup_with(page0) = NULL;
	int ret;

	ret = copy_user_path_at_lookup(&path, upath, flags, false);
	if (ret < 0)
		return ret;
	ret = vfs_at_lookup(dfd, path, flags, lookup_flags, &lookup);
	if (ret < 0)
		return ret;

	ret = vfs_inode_permission(lookup.inode, (uint32_t)mode);

	return ret;
}

static int sys_inode_setattr_path(int dfd, const char *upath, int flags,
				  uint32_t lookup_flags,
				  const struct vfs_inode_attrs *attrs)
{
	struct vfs_at_lookup_result lookup __cleanup_with(vfs_at_lookup) = {};
	char *path __cleanup_with(page0) = NULL;
	int ret;

	ret = copy_user_path_at_lookup(&path, upath, flags, false);
	if (ret < 0)
		return ret;
	ret = vfs_at_lookup(dfd, path, flags, lookup_flags, &lookup);
	if (ret < 0)
		return ret;

	return vfs_inode_setattr(lookup.inode, attrs);
}

static int filldir64(void *arg, const char *name, size_t namelen, uint64_t ino,
		     uint8_t type, loff_t off)
{
	struct getdents_ctx *ctx = arg;
	struct linux_dirent64 dirent;
	char *dst;
	size_t name_off = offsetof(struct linux_dirent64, d_name);
	size_t reclen;

	if (namelen > VFS_NAME_MAX)
		return -EINVAL;

	reclen = name_off + namelen + 1;
	reclen = (reclen + 7) & ~7UL;
	if (ctx->written + reclen > ctx->count)
		return -EINVAL;

	dirent.d_ino = ino;
	dirent.d_off = off;
	dirent.d_reclen = (uint16_t)reclen;
	dirent.d_type = vfs_type_to_dirent(type);

	dst = ctx->dirp + ctx->written;
	memset(dst, 0, reclen);
	memcpy(dst, &dirent, name_off);
	memcpy(dst + name_off, name, namelen);

	ctx->written += reclen;
	return 0;
}

ssize_t sys_openat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	uint32_t flags = (uint32_t)syscall_arg(tf, 2);
	uint32_t mode = (uint32_t)syscall_arg(tf, 3);
	char *path __cleanup_with(page0) = NULL;
	struct path base __cleanup_with(path) = {};
	int ret;

	ret = copy_user_path_at(dfd, upath, &path, &base);
	if (ret < 0)
		return ret;

	ret = vfs_openat_path(base.dentry ? &base : NULL, path, flags,
			      apply_umask(mode));
	return ret;
}

ssize_t sys_mkdirat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	uint32_t mode = (uint32_t)syscall_arg(tf, 2);
	char *path __cleanup_with(page0) = NULL;
	struct path base __cleanup_with(path) = {};
	int ret;

	ret = copy_user_path_at(dfd, upath, &path, &base);
	if (ret < 0)
		return ret;

	ret = vfs_mkdir_at_path(base.dentry ? &base : NULL, path,
				apply_umask(mode));
	return ret;
}

ssize_t sys_unlinkat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	int flags = (int)syscall_arg(tf, 2);
	char *path __cleanup_with(page0) = NULL;
	struct path base __cleanup_with(path) = {};
	int ret;

	if (flags & ~AT_REMOVEDIR)
		return -EINVAL;

	ret = copy_user_path_at(dfd, upath, &path, &base);
	if (ret < 0)
		return ret;

	ret = vfs_unlink_at_path(base.dentry ? &base : NULL, path, flags);
	return ret;
}

ssize_t sys_chdir(struct trap_frame *tf)
{
	const char *upath = (const char *)syscall_arg(tf, 0);
	char *path __cleanup_with(page0) = NULL;
	struct vfs_at_lookup_result found __cleanup_with(vfs_at_lookup) = {};
	int ret;

	ret = copy_user_path(&path, upath);
	if (ret < 0)
		return ret;

	ret = vfs_at_lookup(AT_FDCWD, path, 0, 0, &found);
	if (ret < 0)
		return ret;

	ret = vfs_chdir_path(&found.path);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): faccessat
 * Current: checks R_OK/W_OK/X_OK through VFS permissions at the target path.
 * Unsupported errno: invalid mode bits return -EINVAL; real/effective
 * credential differences are not modeled separately.
 * Future: deepen this with the credential model.
 */
ssize_t sys_faccessat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	int mode = (int)syscall_arg(tf, 2);

	if (mode & ~(R_OK | W_OK | X_OK))
		return -EINVAL;

	return sys_faccessat_path(dfd, upath, mode, 0, 0);
}

/*
 * SYSCALL_SUPPORT(B): faccessat2
 * Current: accepts AT_EACCESS, AT_EMPTY_PATH, and AT_SYMLINK_NOFOLLOW.
 * Unsupported errno: unknown flags or mode bits return -EINVAL; credential
 * semantics remain shallow.
 * Future: tie AT_EACCESS behavior to a fuller credential model.
 */
ssize_t sys_faccessat2(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	int mode = (int)syscall_arg(tf, 2);
	int flags = (int)syscall_arg(tf, 3);

	if (mode & ~(R_OK | W_OK | X_OK))
		return -EINVAL;
	if (flags & ~(AT_EACCESS | AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EINVAL;
	return sys_faccessat_path(
		dfd, upath, mode, flags,
		(flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0);
}

/*
 * SYSCALL_SUPPORT(B): fchmodat
 * Current: changes special and permission bits through VFS, following a final
 * symlink as required by the three-argument Linux syscall ABI.
 * Unsupported errno: VFS returns -EPERM unless the caller is root or owns the
 * inode; no mount read-only or immutable inode state exists yet.
 * Future: add filesystem attributes only with matching VFS ownership rules.
 */
ssize_t sys_fchmodat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	struct vfs_inode_attrs attrs = {
		.valid = VFS_ATTR_MODE,
		.mode = (uint32_t)syscall_arg(tf, 2),
	};

	return sys_inode_setattr_path(dfd, upath, 0, 0, &attrs);
}

/*
 * SYSCALL_SUPPORT(B): fchownat
 * Current: changes uid and/or gid through VFS; AT_EMPTY_PATH and
 * AT_SYMLINK_NOFOLLOW select existing VFS lookup behavior. The simplified
 * credential model restricts ownership changes to uid 0.
 * Unsupported errno: unknown flags return -EINVAL; non-root ownership or
 * group changes return -EPERM.
 * Future: replace root-only ownership checks with capabilities and
 * supplementary-group semantics.
 */
ssize_t sys_fchownat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	uint32_t uid = (uint32_t)syscall_arg(tf, 2);
	uint32_t gid = (uint32_t)syscall_arg(tf, 3);
	int flags = (int)syscall_arg(tf, 4);
	struct vfs_inode_attrs attrs = {};

	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EINVAL;
	if (uid != UINT32_MAX) {
		attrs.valid |= VFS_ATTR_UID;
		attrs.uid = uid;
	}
	if (gid != UINT32_MAX) {
		attrs.valid |= VFS_ATTR_GID;
		attrs.gid = gid;
	}

	return sys_inode_setattr_path(
		dfd, upath, flags,
		(flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0, &attrs);
}

/*
 * SYSCALL_SUPPORT(B): fchmodat2
 * Current: adds AT_EMPTY_PATH and AT_SYMLINK_NOFOLLOW to fchmodat through the
 * VFS at-path and attribute-mutation interfaces. A nofollow symlink target
 * returns -EOPNOTSUPP, matching Linux VFS mode-mutation policy.
 * Unsupported errno: unknown flags return -EINVAL; credentials and filesystem
 * metadata limits follow fchmodat.
 * Future: deepen the credential and mount-state models with VFS support.
 */
ssize_t sys_fchmodat2(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	int flags = (int)syscall_arg(tf, 3);
	struct vfs_inode_attrs attrs = {
		.valid = VFS_ATTR_MODE,
		.mode = (uint32_t)syscall_arg(tf, 2),
	};

	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EINVAL;

	return sys_inode_setattr_path(
		dfd, upath, flags,
		(flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0, &attrs);
}

ssize_t sys_getcwd(struct trap_frame *tf)
{
	char *ubuf = (char *)syscall_arg(tf, 0);
	size_t size = syscall_arg(tf, 1);
	struct path cwd __cleanup_with(path) = {};
	char *path __cleanup_with(page0) = NULL;
	int ret;

	if (!ubuf || size == 0)
		return -EINVAL;

	path = get_free_page(0, ALLOC_NOWAIT);
	if (!path)
		return -ENOMEM;

	ret = fs_get_cwd_path(current_task()->proc ? current_task()->proc->fs : NULL,
					&cwd);
	if (ret < 0)
		return ret;
	ret = vfs_getcwd_path(&cwd, path, VFS_PATH_MAX);
	if (ret < 0)
		return ret;
	if ((size_t)ret > size)
		return -ERANGE;
	if (!access_ok(ubuf, (size_t)ret))
		return -EFAULT;
	if (copy_to_user(ubuf, path, (size_t)ret) != 0)
		return -EFAULT;

	return ret;
}

ssize_t sys_getdents64(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	char *dirp = (char *)syscall_arg(tf, 1);
	size_t count = syscall_arg(tf, 2);
	struct file *file __cleanup_with(file) = fd_get(fd);
	char kbuf[SYS_FILE_BUF_SIZE];
	struct getdents_ctx ctx;
	loff_t start;
	int ret;
	ssize_t result;

	if (!file)
		return -EBADF;
	if (count == 0)
		return -EINVAL;
	if (!access_ok(dirp, count))
		return -EFAULT;

	start = file->f_pos;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dirp = kbuf;
	ctx.count = sizeof(kbuf);

	while ((size_t)((uintptr_t)dirp - syscall_arg(tf, 1)) < count) {
		size_t done = (size_t)((uintptr_t)dirp - syscall_arg(tf, 1));
		size_t chunk = count - done;
		if (chunk > sizeof(kbuf))
			chunk = sizeof(kbuf);

		memset(kbuf, 0, sizeof(kbuf));
		ctx.dirp = kbuf;
		ctx.count = chunk;
		ctx.written = 0;

		ret = vfs_readdir(file, &ctx, filldir64);
		if (ret < 0) {
			if (ctx.written == 0) {
				file->f_pos = start;
				return done ? (ssize_t)done : ret;
			}
		}
		if (ctx.written == 0)
			break;

		if (copy_to_user(dirp, kbuf, ctx.written) != 0) {
			file->f_pos = start;
			return -EFAULT;
		}

		dirp += ctx.written;
		start = file->f_pos;
		if (ret < 0)
			break;
	}

	result = (ssize_t)((uintptr_t)dirp - syscall_arg(tf, 1));
	return result;
}

ssize_t sys_readlinkat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	char *ubuf = (char *)syscall_arg(tf, 2);
	size_t bufsiz = (size_t)syscall_arg(tf, 3);
	char *path __cleanup_with(page0) = NULL;
	char *link __cleanup_with(page0) = NULL;
	size_t link_size;
	struct vfs_at_lookup_result found __cleanup_with(vfs_at_lookup) = {};
	int len;
	int ret;

	if (bufsiz == 0)
		return -EINVAL;
	if (!ubuf || !access_ok(ubuf, bufsiz))
		return -EFAULT;


	ret = copy_user_path(&path, upath);
	if (ret < 0)
		return ret;

	ret = vfs_at_lookup(dfd, path, 0, LOOKUP_NOFOLLOW,
				    &found);
	if (ret < 0)
		return ret;

	link_size = bufsiz < VFS_PATH_MAX ? bufsiz : VFS_PATH_MAX;
	link = get_free_page(0, ALLOC_NOWAIT);
	if (!link)
		return -ENOMEM;

	len = vfs_readlink(found.path.dentry, link, link_size);
	if (len < 0)
		return len;

	if ((size_t)len > bufsiz)
		len = (int)bufsiz;
	if (copy_to_user(ubuf, link, (size_t)len) != 0)
		return -EFAULT;

	return len;
}

/*
 * SYSCALL_SUPPORT(B): symlinkat
 * Current: delegates symlink creation to VFS/ext2 after copying both paths.
 * Unsupported errno: no syscall-layer feature gate; path and filesystem errors
 * are returned by VFS/ext2.
 * Future: add long-symlink and filesystem-boundary coverage.
 */
ssize_t sys_symlinkat(struct trap_frame *tf)
{
	const char *utarget = (const char *)syscall_arg(tf, 0);
	int newdfd = (int)syscall_arg(tf, 1);
	const char *ulinkpath = (const char *)syscall_arg(tf, 2);
	char *target __cleanup_with(page0) = NULL;
	char *linkpath __cleanup_with(page0) = NULL;
	struct path base __cleanup_with(path) = {};
	int ret;

	ret = copy_user_path(&target, utarget);
	if (ret < 0)
		return ret;
	ret = copy_user_path_at(newdfd, ulinkpath, &linkpath, &base);
	if (ret < 0)
		return ret;

	ret = vfs_symlink_at_path(base.dentry ? &base : NULL, target, linkpath);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): linkat
 * Current: supports AT_SYMLINK_FOLLOW and otherwise delegates hard-link policy
 * to VFS.
 * Unsupported errno: unknown flags return -EINVAL; cross-mount and directory
 * cases use VFS errno.
 * Future: document the linkat errno table.
 */
ssize_t sys_linkat(struct trap_frame *tf)
{
	int olddfd = (int)syscall_arg(tf, 0);
	const char *uoldpath = (const char *)syscall_arg(tf, 1);
	int newdfd = (int)syscall_arg(tf, 2);
	const char *unewpath = (const char *)syscall_arg(tf, 3);
	int flags = (int)syscall_arg(tf, 4);
	char *oldpath __cleanup_with(page0) = NULL;
	char *newpath __cleanup_with(page0) = NULL;
	struct path new_base __cleanup_with(path) = {};
	struct vfs_at_lookup_result old_path_found
		__cleanup_with(vfs_at_lookup) = {};
	int ret;

	if (flags & ~AT_SYMLINK_FOLLOW)
		return -EINVAL;

	ret = copy_user_path(&oldpath, uoldpath);
	if (ret < 0)
		return ret;
	ret = vfs_at_lookup(olddfd, oldpath, 0,
				    (flags & AT_SYMLINK_FOLLOW) ? 0 : LOOKUP_NOFOLLOW,
				    &old_path_found);
	if (ret < 0)
		return ret;

	ret = copy_user_path_at(newdfd, unewpath, &newpath, &new_base);
	if (ret < 0)
		return ret;
	ret = vfs_link_at_path(old_path_found.path.dentry,
			       new_base.dentry ? &new_base : NULL, newpath);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): mknodat
 * Current: delegates node creation to VFS with the caller mode after umask.
 * Unsupported errno: no syscall-layer mode filter; unsupported node or device
 * cases are reported by VFS/filesystem code.
 * Future: document the supported chr, blk, and fifo range.
 */
ssize_t sys_mknod(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	uint32_t mode = (uint32_t)syscall_arg(tf, 2);
	dev_t dev = (dev_t)syscall_arg(tf, 3);
	char *path __cleanup_with(page0) = NULL;
	struct path base __cleanup_with(path) = {};
	int ret;

	ret = copy_user_path_at(dfd, upath, &path, &base);
	if (ret < 0)
		return ret;

	ret = vfs_mknod_at_path(base.dentry ? &base : NULL, path,
				apply_umask(mode), dev);
	return ret;
}

/*
 * SYSCALL_SUPPORT(C): umount2
 * Current: delegates to the single-namespace VFS unmount path for mounted
 * roots.
 * Unsupported errno: any nonzero flag returns -EINVAL; root or busy mounts
 * return -EBUSY; non-mount targets return -EINVAL.
 * Future: keep C until lazy/force/no-follow and namespace semantics exist.
 */
ssize_t sys_umount2(struct trap_frame *tf)
{
	const char *utarget = (const char *)syscall_arg(tf, 0);
	int flags = (int)syscall_arg(tf, 1);
	char *target __cleanup_with(page0) = NULL;
	int ret;

	ret = copy_user_path(&target, utarget);
	if (ret < 0)
		return ret;

	ret = vfs_umount(target, flags);
	return ret;
}

/*
 * SYSCALL_SUPPORT(C): mount
 * Current: supports a minimal single-namespace ext2 block-device-to-directory
 * VFS mount path with explicit filesystem type and zero flags.
 * Unsupported errno: nonzero flags return -EINVAL; unknown filesystem returns
 * -ENODEV; non-block sources return -ENOTBLK; non-directory targets return
 * -ENOTDIR; duplicate mountpoints return -EBUSY.
 * Future: keep C until bind, remount, move, propagation, read-only, or mount
 * namespace semantics exist.
 */
ssize_t sys_mount(struct trap_frame *tf)
{
	const char *usource = (const char *)syscall_arg(tf, 0);
	const char *utarget = (const char *)syscall_arg(tf, 1);
	const char *utype = (const char *)syscall_arg(tf, 2);
	unsigned long flags = (unsigned long)syscall_arg(tf, 3);
	const void *data = (const void *)syscall_arg(tf, 4);
	char *source __cleanup_with(page0) = NULL;
	char *target __cleanup_with(page0) = NULL;
	char *type __cleanup_with(page0) = NULL;
	int ret;

	ret = copy_user_path(&source, usource);
	if (ret < 0)
		return ret;
	ret = copy_user_path(&target, utarget);
	if (ret < 0)
		return ret;
	ret = copy_user_path(&type, utype);
	if (ret < 0)
		return ret;

	ret = vfs_mount(source, target, type, flags, data);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): renameat2
 * Current: supports plain rename and RENAME_NOREPLACE through VFS.
 * Unsupported errno: unknown flags return -EINVAL.
 * Future: document flag policy before adding exchange or whiteout support.
 */
ssize_t sys_renameat2(struct trap_frame *tf)
{
	int old_dfd = (int)syscall_arg(tf, 0);
	const char *uold_path = (const char *)syscall_arg(tf, 1);
	int new_dfd = (int)syscall_arg(tf, 2);
	const char *unew_path = (const char *)syscall_arg(tf, 3);
	unsigned int flags = (unsigned int)syscall_arg(tf, 4);
	char *old_path __cleanup_with(page0) = NULL;
	char *new_path __cleanup_with(page0) = NULL;
	struct path old_base __cleanup_with(path) = {};
	struct path new_base __cleanup_with(path) = {};
	int ret;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	ret = copy_user_path_at(old_dfd, uold_path, &old_path, &old_base);
	if (ret < 0)
		return ret;

	ret = copy_user_path_at(new_dfd, unew_path, &new_path, &new_base);
	if (ret < 0)
		return ret;

	ret = vfs_rename_at_path(old_base.dentry ? &old_base : NULL, old_path,
				 new_base.dentry ? &new_base : NULL, new_path,
				 flags);
	return ret;
}

static int sys_utimensat_read_times(const struct timespec *utimes,
				    struct timespec ktimes[2], bool set_time[2])
{
	struct timespec now;

	mtime_to_timespec(timer_now(), &now);
	if (!utimes) {
		ktimes[0] = now;
		ktimes[1] = now;
		set_time[0] = true;
		set_time[1] = true;
		return 0;
	}
	if (copy_from_user(ktimes, utimes, sizeof(struct timespec) * 2) != 0)
		return -EFAULT;

	for (int i = 0; i < 2; i++) {
		set_time[i] = true;
		if (ktimes[i].tv_nsec == UTIME_OMIT) {
			set_time[i] = false;
			continue;
		}
		if (ktimes[i].tv_nsec == UTIME_NOW) {
			ktimes[i] = now;
			continue;
		}
		if (ktimes[i].tv_sec < 0 || ktimes[i].tv_sec > UINT32_MAX ||
		    ktimes[i].tv_nsec < 0 || ktimes[i].tv_nsec >= 1000000000LL)
			return -EINVAL;
	}

	return 0;
}

/*
 * SYSCALL_SUPPORT(B): utimensat
 * Current: supports NOW/OMIT, AT_EMPTY_PATH, and AT_SYMLINK_NOFOLLOW.
 * Unsupported errno: unknown flags or invalid timespec fields return -EINVAL;
 * permission and ctime behavior remains shallow.
 * Future: add permission and ctime coverage.
 */
ssize_t sys_utimensat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	const struct timespec *utimes = (const struct timespec *)syscall_arg(tf, 2);
	int flags = (int)syscall_arg(tf, 3);
	struct timespec ktimes[2];
	bool set_time[2];
	struct vfs_at_lookup_result lookup __cleanup_with(vfs_at_lookup) = {};
	char *path __cleanup_with(page0) = NULL;
	int ret;

	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EINVAL;

	ret = sys_utimensat_read_times(utimes, ktimes, set_time);
	if (ret < 0)
		return ret;

	ret = copy_user_path_at_lookup(&path, upath, flags, true);
	if (ret < 0)
		return ret;
	ret = vfs_at_lookup(dfd, path, flags,
			    (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0,
			    &lookup);
	if (ret < 0)
		return ret;

	ret = vfs_inode_set_timestamps(lookup.inode, ktimes[0].tv_sec,
				       ktimes[1].tv_sec, set_time[0],
				       set_time[1]);
	return ret;
}

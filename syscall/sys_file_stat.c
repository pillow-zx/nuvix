/*
 * syscall/sys_file_stat.c - stat/statfs 元数据查询系统调用
 */

#include <nuvix/fdtable.h>
#include <nuvix/blkdev.h>
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
#include <nuvix/task.h>
#include <nuvix/vfs.h>
#include <nuvix/page.h>
#include <nuvix/trap.h>

#include "sys_file_internal.h"

ssize_t sys_fstat(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	struct stat *ustat = (struct stat *)syscall_arg(tf, 1);
	struct file *file __cleanup_with(file) = fd_get(fd);
	struct stat st;
	int ret;

	if (!file)
		return -EBADF;
	if (!access_ok(ustat, sizeof(*ustat)))
		return -EFAULT;

	ret = vfs_stat_file(file, &st);
	if (ret == 0)
		ret = copy_to_user(ustat, &st, sizeof(st)) != 0 ? -EFAULT : 0;

	return ret;
}

ssize_t sys_newfstatat(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	struct stat *ustat = (struct stat *)syscall_arg(tf, 2);
	int flags = (int)syscall_arg(tf, 3);
	struct vfs_at_lookup_result lookup __cleanup_with(vfs_at_lookup) = {};
	char *path __cleanup_with(page0) = NULL;
	struct stat st;
	int ret;

	if (!ustat || !access_ok(ustat, sizeof(*ustat)))
		return -EFAULT;
	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
		return -EINVAL;

	ret = copy_user_path_at_lookup(&path, upath, flags, true);
	if (ret < 0)
		return ret;
	ret = vfs_at_lookup(dfd, path, flags,
			    (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0,
			    &lookup);
	if (ret < 0)
		return ret;

	ret = vfs_stat_inode(lookup.inode, &st);
	if (ret < 0)
		return ret;
	if (copy_to_user(ustat, &st, sizeof(st)) != 0)
		return -EFAULT;

	return 0;
}

static void statx_timestamp_from_stat(struct statx_timestamp *dst,
				      int64_t sec, uint64_t nsec)
{
	dst->tv_sec = sec;
	dst->tv_nsec = (uint32_t)nsec;
	dst->__reserved = 0;
}

static void statx_from_stat(const struct stat *st, struct statx *stx)
{
	memset(stx, 0, sizeof(*stx));
	stx->stx_mask = STATX_BASIC_STATS;
	stx->stx_blksize = st->st_blksize;
	stx->stx_nlink = st->st_nlink;
	stx->stx_uid = st->st_uid;
	stx->stx_gid = st->st_gid;
	stx->stx_mode = (uint16_t)st->st_mode;
	stx->stx_ino = st->st_ino;
	stx->stx_size = (uint64_t)st->st_size;
	stx->stx_blocks = st->st_blocks;
	statx_timestamp_from_stat(&stx->stx_atime, st->st_atime_sec,
				  st->st_atime_nsec);
	statx_timestamp_from_stat(&stx->stx_mtime, st->st_mtime_sec,
				  st->st_mtime_nsec);
	statx_timestamp_from_stat(&stx->stx_ctime, st->st_ctime_sec,
				  st->st_ctime_nsec);
	stx->stx_rdev_major = MAJOR(st->st_rdev);
	stx->stx_rdev_minor = (uint32_t)(st->st_rdev & ((1u << MINORBITS) - 1));
	stx->stx_dev_major = MAJOR(st->st_dev);
	stx->stx_dev_minor = (uint32_t)(st->st_dev & ((1u << MINORBITS) - 1));
}

/*
 * SYSCALL_SUPPORT(B): statx
 * Current: reports real STATX_BASIC_STATS fields converted from VFS stat data.
 * Unsupported errno: reserved mask bits and unsupported AT_STATX flag
 * combinations return -EINVAL; unsupported field request bits are accepted as
 * probes but are not set in stx_mask.
 * Future: add btime, mount id, DIO alignment, or attribute bits only when the
 * backing VFS/filesystem state exists.
 */
ssize_t sys_statx(struct trap_frame *tf)
{
	int dfd = (int)syscall_arg(tf, 0);
	const char *upath = (const char *)syscall_arg(tf, 1);
	int flags = (int)syscall_arg(tf, 2);
	uint32_t mask = (uint32_t)syscall_arg(tf, 3);
	struct statx *ustatx = (struct statx *)syscall_arg(tf, 4);
	struct vfs_at_lookup_result lookup __cleanup_with(vfs_at_lookup) = {};
	char *path __cleanup_with(page0) = NULL;
	struct stat st;
	struct statx stx;
	int ret;

	if (!ustatx || !access_ok(ustatx, sizeof(*ustatx)))
		return -EFAULT;
	if (mask & STATX__RESERVED)
		return -EINVAL;
	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW |
		      AT_STATX_SYNC_TYPE))
		return -EINVAL;
	if ((flags & AT_STATX_SYNC_TYPE) != AT_STATX_FORCE_SYNC &&
	    (flags & AT_STATX_SYNC_TYPE) != AT_STATX_DONT_SYNC &&
	    (flags & AT_STATX_SYNC_TYPE) != 0)
		return -EINVAL;

	ret = copy_user_path_at_lookup(&path, upath, flags, true);
	if (ret < 0)
		return ret;
	ret = vfs_at_lookup(dfd, path, flags,
			    (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0,
			    &lookup);
	if (ret < 0)
		return ret;

	ret = vfs_stat_inode(lookup.inode, &st);
	if (ret < 0)
		return ret;

	statx_from_stat(&st, &stx);
	if (copy_to_user(ustatx, &stx, sizeof(stx)) != 0)
		return -EFAULT;

	return 0;
}

/*
 * SYSCALL_SUPPORT(B): statfs64
 * Current: reports the mounted superblock statfs data for a resolved path.
 * Unsupported errno: missing mount/superblock returns -EINVAL; detailed field
 * semantics depend on the filesystem. ext2 fills block/inode counts, name
 * length, UUID-derived fsid, and zero mount flags.
 * Future: extend per-filesystem statfs fields only when the backing
 * filesystem owns the corresponding state.
 */
ssize_t sys_statfs64(struct trap_frame *tf)
{
	const char *upath = (const char *)syscall_arg(tf, 0);
	struct statfs64 *ubuf = (struct statfs64 *)syscall_arg(tf, 1);
	char *path __cleanup_with(page0) = NULL;
	struct vfs_at_lookup_result found __cleanup_with(vfs_at_lookup) = {};
	struct statfs64 st;
	int ret;

	if (!ubuf || !access_ok(ubuf, sizeof(*ubuf)))
		return -EFAULT;

	ret = copy_user_path(&path, upath);
	if (ret < 0)
		return ret;

	ret = vfs_at_lookup(AT_FDCWD, path, 0, 0, &found);
	if (ret < 0)
		return ret;
	if (!found.path.mnt || !found.path.mnt->mnt_sb)
		return -EINVAL;

	ret = vfs_statfs(found.path.mnt->mnt_sb, &st);
	if (ret < 0)
		return ret;
	if (copy_to_user(ubuf, &st, sizeof(st)) != 0)
		return -EFAULT;

	return 0;
}

/*
 * SYSCALL_SUPPORT(B): fstatfs64
 * Current: reports the mounted superblock statfs data for an open file.
 * Unsupported errno: bad fd returns -EBADF; missing mount/superblock returns
 * -EINVAL; field semantics depend on the filesystem. ext2 mirrors statfs64
 * path semantics.
 * Future: track any new per-filesystem statfs fields with statfs64.
 */
ssize_t sys_fstatfs64(struct trap_frame *tf)
{
	int fd = (int)syscall_arg(tf, 0);
	struct statfs64 *ubuf = (struct statfs64 *)syscall_arg(tf, 1);
	struct file *file __cleanup_with(file) = NULL;
	struct statfs64 st;
	int ret;

	if (!ubuf || !access_ok(ubuf, sizeof(*ubuf)))
		return -EFAULT;

	file = fd_get(fd);
	if (!file)
		return -EBADF;
	if (!file->f_path.mnt || !file->f_path.mnt->mnt_sb)
		return -EINVAL;

	ret = vfs_statfs(file->f_path.mnt->mnt_sb, &st);
	if (ret < 0)
		return ret;
	if (copy_to_user(ubuf, &st, sizeof(st)) != 0)
		return -EFAULT;

	return 0;
}

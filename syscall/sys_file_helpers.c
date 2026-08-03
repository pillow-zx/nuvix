/*
 * syscall/sys_file_helpers.c - shared file syscall ABI helpers
 */

#include <kernel/buddy.h>
#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/fdtable.h>
#include <kernel/fs.h>
#include <kernel/fs_struct.h>
#include <kernel/mm.h>
#include <kernel/stat.h>
#include <kernel/task.h>
#include <kernel/vfs.h>
#include <kernel/page.h>

#include "sys_file_internal.h"

static_assert(VFS_PATH_MAX <= PAGE_SIZE,
	      "syscall path buffers are allocated as one page");

int copy_user_path(char **pathp, const char *user)
{
	char *dst __cleanup_with(page0) = NULL;
	ssize_t len;

	if (pathp)
		*pathp = NULL;
	if (!pathp)
		return -EINVAL;
	if (!user)
		return -EFAULT;

	dst = get_free_page(0, ALLOC_NOWAIT);
	if (!dst)
		return -ENOMEM;

	len = strncpy_from_user(dst, user, VFS_PATH_MAX);
	if (len < 0)
		return (int)len;
	if (len == 0)
		return -ENOENT;

	*pathp = cleanup_take_ptr(dst);
	return 0;
}

int copy_user_path_allow_empty(char **pathp, const char *user)
{
	char *dst __cleanup_with(page0) = NULL;
	ssize_t len;

	if (pathp)
		*pathp = NULL;
	if (!pathp)
		return -EINVAL;
	if (!user)
		return -EFAULT;

	dst = get_free_page(0, ALLOC_NOWAIT);
	if (!dst)
		return -ENOMEM;

	len = strncpy_from_user(dst, user, VFS_PATH_MAX);
	if (len < 0)
		return (int)len;
	*pathp = cleanup_take_ptr(dst);
	return 0;
}

int copy_user_path_at_lookup(char **pathp, const char *user, int at_flags,
			     bool null_is_empty)
{
	if (!pathp)
		return -EINVAL;
	if (!user) {
		*pathp = NULL;
		return (at_flags & AT_EMPTY_PATH) && null_is_empty ? 0 : -EFAULT;
	}
	return copy_user_path_allow_empty(pathp, user);
}

int copy_user_path_at(int dfd, const char *user, char **pathp,
		      struct path *basep)
{
	int ret;

	ret = copy_user_path(pathp, user);
	if (ret < 0)
		return ret;

	ret = vfs_at_base_path(dfd, *pathp, basep);
	return ret;
}

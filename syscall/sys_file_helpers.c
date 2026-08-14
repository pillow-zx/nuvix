/*
 * syscall/sys_file_helpers.c - shared file syscall ABI helpers
 */

#include <nuvix/buddy.h>
#include <nuvix/compiler.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/fs_struct.h>
#include <nuvix/mm.h>
#include <nuvix/stat.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>
#include <nuvix/page.h>

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

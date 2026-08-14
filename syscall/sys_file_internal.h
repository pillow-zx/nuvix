/*
 * syscall/sys_file_internal.h - shared helpers between sys_file_*.c
 */
#ifndef SYS_FILE_INTERNAL_H
#define SYS_FILE_INTERNAL_H

#include <nuvix/buddy.h>
#include <nuvix/cleanup.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/types.h>
#include <nuvix/vfs.h>

#define SYS_FILE_BUF_SIZE 256
#define SYS_IOV_MAX	  64

CLEANUP_DEFINE(path, struct path, if (_T.dentry || _T.mnt) path_put(&_T));
CLEANUP_DEFINE(vfs_at_lookup, struct vfs_at_lookup_result,
	       vfs_at_lookup_put(&_T));

int copy_user_path(char **pathp, const char *user);
int copy_user_path_allow_empty(char **pathp, const char *user);
int copy_user_path_at_lookup(char **pathp, const char *user, int at_flags,
			     bool null_is_empty);
int copy_user_path_at(int dfd, const char *user, char **pathp,
			      struct path *basep);

#endif

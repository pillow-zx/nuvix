#ifndef _CUTEOS_KERNEL_FS_STRUCT_H
#define _CUTEOS_KERNEL_FS_STRUCT_H

/**
 * @file fs_struct.h
 * @brief 可共享的进程文件系统上下文。
 */

#include <kernel/fs.h>
#include <kernel/refcount.h>
#include <kernel/mutex.h>
#include <kernel/types.h>

struct proc_struct;

/**
 * @struct fs_struct
 * @brief cwd/root/umask state shared by tasks using CLONE_FS.
 *
 * @par Fields
 * - @c refcount: References from tasks sharing this context.
 * - @c lock: Serializes root/cwd/umask updates.
 * - @c root: Filesystem root for absolute path lookup.
 * - @c cwd: Current working directory for relative lookup.
 * - @c umask: Process umask applied to newly created objects.
 */
struct fs_struct {
	refcount_t refcount;
	mutex_t lock;
	struct path root;
	struct path cwd;
	uint32_t umask;
};

/**
 * @brief Allocate a filesystem context.
 * @return New fs_struct, or NULL.
 */
__must_check
struct fs_struct *fs_alloc(void);

/**
 * @brief Duplicate a filesystem context for fork without CLONE_FS.
 * @param old Source context.
 * @return New context with referenced paths, or NULL.
 */
__must_check
struct fs_struct *fs_dup(struct fs_struct *old);
void fs_get(struct fs_struct *fs);
void fs_put(struct fs_struct *fs);

/**
 * @brief Snapshot the current root path.
 * @param fs Filesystem context.
 * @param path Output path receiving references.
 * @return 0 on success, or a negative errno.
 */
__must_check
int fs_get_root_path(struct fs_struct *fs, struct path *path);

/**
 * @brief Snapshot the current working directory path.
 * @param fs Filesystem context.
 * @param path Output path receiving references.
 * @return 0 on success, or a negative errno.
 */
__must_check
int fs_get_cwd_path(struct fs_struct *fs, struct path *path);

/**
 * @brief Replace the current working directory.
 * @param fs Filesystem context.
 * @param path New cwd path.
 * @return 0 on success, or a negative errno.
 */
__must_check
int fs_set_cwd_path(struct fs_struct *fs, const struct path *path);

__must_check
uint32_t fs_get_umask(struct fs_struct *fs);

uint32_t fs_set_umask(struct fs_struct *fs, uint32_t mask);

void fs_set_root_if_empty(struct fs_struct *fs, struct dentry *root);

__must_check
int init_fs(struct proc_struct *proc);

__must_check
int copy_fs(const struct proc_struct *source, struct proc_struct *dest,
		   bool share);

void exit_fs(struct proc_struct *proc);

#endif

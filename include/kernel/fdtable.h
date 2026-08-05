#ifndef _CUTEOS_KERNEL_FDTABLE_H
#define _CUTEOS_KERNEL_FDTABLE_H

/**
 * @file fdtable.h
 * @brief file 引用计数与可共享 fd 表。
 */

#include <kernel/fs.h>
#include <kernel/refcount.h>
#include <kernel/mutex.h>
#include <kernel/cleanup.h>

/**
 * @struct files_struct
 * @brief Per-task or shared file-descriptor table.
 *
 * @par Fields
 * - @c refcount: References from tasks sharing this table.
 * - @c lock: Serializes fd slot and close-on-exec updates.
 * - @c close_on_exec: Bit mask of fds closed by execve.
 * - @c fd: Open file pointers indexed by fd number.
 */
struct files_struct {
	refcount_t refcount;
	mutex_t lock;
	unsigned long close_on_exec;
	struct file *fd[NR_OPEN];
};

struct proc_struct;

/**
 * @brief Allocate an anonymous file object.
 * @param f_op Operations for the file.
 * @param mode FMODE_* access bits.
 * @param private_data File-type-private state.
 * @return New referenced file, or NULL.
 */
__must_check
struct file *file_alloc(const struct file_operations *f_op, uint32_t mode, void *private_data);

/**
 * @brief Allocate a path-backed file object.
 * @param path VFS path pinned by the file.
 * @param flags Linux O_* status flags.
 * @param mode FMODE_* access bits.
 * @return New referenced file, or NULL.
 */
__must_check
struct file *file_alloc_path(const struct path *path, uint32_t flags, uint32_t mode);

void file_get(struct file *file);

void file_put(struct file *file);

/**
 * @brief Allocate an empty fdtable.
 * @return New files_struct, or NULL.
 */
__must_check
struct files_struct *files_alloc(void);

/**
 * @brief Duplicate an fdtable for fork without CLONE_FILES.
 * @param old Source table.
 * @return New table with referenced files, or NULL.
 */
__must_check
struct files_struct *files_dup(struct files_struct *old);
void files_get(struct files_struct *files);
void files_put(struct files_struct *files);
void files_install_standard_fds(struct files_struct *files);
void files_close_on_exec(struct files_struct *files);

/**
 * @brief Prepare a private fdtable for an exec transaction.
 * @param proc Proc replacing its image.
 * @param prepared Receives the uncommitted fdtable.
 * @return 0, or a negative errno.
 *
 * The current proc fdtable is not modified.  The caller must either commit or
 * abort the returned table.
 */
__must_check
int files_prepare_exec(const struct proc_struct *proc,
			       struct files_struct **prepared);
void files_commit_exec(struct proc_struct *proc,
			       struct files_struct *prepared);
void files_abort_exec(struct files_struct *prepared);

/**
 * @brief Install a referenced file in the lowest free fd slot.
 * @param file File to install.
 * @return Allocated fd, or a negative errno.
 */
__must_check
int fd_alloc(struct file *file);

/**
 * @brief Install a file descriptor with Linux fd flags.
 * @param file File to install.
 * @param flags O_CLOEXEC-compatible flags.
 * @return Allocated fd, or a negative errno.
 */
__must_check
int fd_alloc_flags(struct file *file, int flags);

/**
 * @brief Get a referenced file from the current task fdtable.
 * @param fd File descriptor number.
 * @return Referenced file, or NULL.
 */
__must_check
struct file *fd_get(int fd);

__must_check
struct file *fd_get_checked(int fd);

__must_check
int fd_get_close_on_exec(int fd);

__must_check
int fd_set_close_on_exec(int fd, bool close_on_exec);

int fd_close(int fd);

__must_check
int fd_dup(int oldfd);

__must_check
int fd_dup_from(int oldfd, unsigned long minfd, int cloexec);

__must_check
int fd_dup2(int oldfd, int newfd, int cloexec);

__must_check
int init_files(struct proc_struct *proc);
__must_check
int copy_files(const struct proc_struct *source, struct proc_struct *dest,
		       bool share);

void close_files(struct proc_struct *proc);

CLEANUP_DEFINE(file, struct file *, if (_T) file_put(_T));
#endif

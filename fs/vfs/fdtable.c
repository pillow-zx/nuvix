/*
 * fs/vfs/fdtable.c - file descriptor table management
 */

#include <nuvix/errno.h>
#include <nuvix/blkdev.h>
#include <nuvix/fdtable.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>

static struct files_struct *current_files(void)
{
	return current_task()->files;
}

struct files_struct *files_alloc(void)
{
	struct files_struct *files = kmalloc(sizeof(*files), ALLOC_NOWAIT);

	if (!files)
		return NULL;

	memset(files, 0, sizeof(*files));
	refcount_set(&files->refcount, 1);
	mutex_init(&files->lock, LOCK_RANK_FILES_FDTABLE, LOCK_IRQ_TASK_ONLY);
	return files;
}

struct files_struct *files_dup(struct files_struct *old)
{
	struct files_struct *files = files_alloc();

	if (!files)
		return NULL;
	if (!old)
		return files;

	mutex_lock(&old->lock);
	files->close_on_exec = old->close_on_exec;
	for (int fd = 0; fd < NR_OPEN; fd++) {
		files->fd[fd] = old->fd[fd];
		file_get(files->fd[fd]);
	}
	mutex_unlock(&old->lock);

	return files;
}

void files_get(struct files_struct *files)
{
	if (files)
		refcount_inc(&files->refcount);
}

void files_put(struct files_struct *files)
{
	if (!files)
		return;

	if (!refcount_dec_and_test(&files->refcount))
		return;

	for (int fd = 0; fd < NR_OPEN; fd++) {
		struct file *file = files->fd[fd];

		files->fd[fd] = NULL;
		file_put(file);
	}
	kfree(files);
}

void files_install_standard_fds(struct files_struct *files)
{
	const struct file_operations *fops;

	if (!files)
		return;

	fops = vfs_chrdev_fops(MKDEV(5, 1));
	BUG_ON(!fops);

	files->fd[KERN_STDIN] = file_alloc(fops, FMODE_READ, NULL);
	files->fd[KERN_STDOUT] = file_alloc(fops, FMODE_WRITE, NULL);
	files->fd[KERN_STDERR] = file_alloc(fops, FMODE_WRITE, NULL);
	BUG_ON(!files->fd[KERN_STDIN] || !files->fd[KERN_STDOUT] ||
	       !files->fd[KERN_STDERR]);
}

int fd_alloc_flags(struct file *file, int flags)
{
	struct files_struct *files = current_files();

	if (!file)
		return -EINVAL;
	if (!files)
		return -EBADF;

	mutex_lock(&files->lock);
	for (int fd = 0; fd < NR_OPEN; fd++) {
		if (!files->fd[fd]) {
			files->fd[fd] = file;
			if (flags & O_CLOEXEC)
				files->close_on_exec |=
					(1UL << (unsigned int)fd);
			else
				files->close_on_exec &=
					~(1UL << (unsigned int)fd);
			mutex_unlock(&files->lock);
			return fd;
		}
	}
	mutex_unlock(&files->lock);

	return -EMFILE;
}

int fd_alloc(struct file *file)
{
	return fd_alloc_flags(file, 0);
}

struct file *fd_get_checked(int fd)
{
	struct files_struct *files = current_files();
	struct file *file;

	if (!files || fd < 0 || fd >= NR_OPEN)
		return NULL;

	mutex_lock(&files->lock);
	file = files->fd[fd];
	file_get(file);
	mutex_unlock(&files->lock);
	return file;
}

struct file *fd_get(int fd)
{
	return fd_get_checked(fd);
}

int fd_get_close_on_exec(int fd)
{
	struct files_struct *files = current_files();
	int ret;

	if (!files || fd < 0 || fd >= NR_OPEN)
		return -EBADF;

	mutex_lock(&files->lock);
	if (!files->fd[fd])
		ret = -EBADF;
	else
		ret = !!(files->close_on_exec & (1UL << (unsigned int)fd));
	mutex_unlock(&files->lock);

	return ret;
}

int fd_set_close_on_exec(int fd, bool close_on_exec)
{
	struct files_struct *files = current_files();

	if (!files || fd < 0 || fd >= NR_OPEN)
		return -EBADF;

	mutex_lock(&files->lock);
	if (!files->fd[fd]) {
		mutex_unlock(&files->lock);
		return -EBADF;
	}
	if (close_on_exec)
		files->close_on_exec |= (1UL << (unsigned int)fd);
	else
		files->close_on_exec &= ~(1UL << (unsigned int)fd);
	mutex_unlock(&files->lock);

	return 0;
}

void files_close_on_exec(struct files_struct *files)
{
	struct file *closing[NR_OPEN];
	unsigned long cloexec;

	if (!files)
		return;

	memset(closing, 0, sizeof(closing));
	mutex_lock(&files->lock);
	cloexec = files->close_on_exec;
	files->close_on_exec = 0;
	for (int fd = 0; fd < NR_OPEN && cloexec; fd++, cloexec >>= 1) {
		if (!(cloexec & 1))
			continue;
		closing[fd] = files->fd[fd];
		files->fd[fd] = NULL;
	}
	mutex_unlock(&files->lock);

	for (int fd = 0; fd < NR_OPEN; fd++)
		file_put(closing[fd]);
}

int fd_close(int fd)
{
	struct files_struct *files = current_files();
	struct file *file;

	if (!files || fd < 0 || fd >= NR_OPEN)
		return -EBADF;

	mutex_lock(&files->lock);
	file = files->fd[fd];
	if (!file) {
		mutex_unlock(&files->lock);
		return -EBADF;
	}

	files->fd[fd] = NULL;
	files->close_on_exec &= ~(1UL << (unsigned int)fd);
	mutex_unlock(&files->lock);
	file_put(file);

	return 0;
}

int fd_dup(int oldfd)
{
	struct files_struct *files = current_files();
	struct file *file = fd_get(oldfd);
	int newfd = -EMFILE;

	if (!file)
		return -EBADF;
	if (!files) {
		file_put(file);
		return -EBADF;
	}

	mutex_lock(&files->lock);
	for (int fd = 0; fd < NR_OPEN; fd++) {
		if (!files->fd[fd]) {
			file_get(file);
			files->fd[fd] = file;
			files->close_on_exec &= ~(1UL << (unsigned int)fd);
			newfd = fd;
			break;
		}
	}
	mutex_unlock(&files->lock);

	file_put(file);
	return newfd;
}

int fd_dup_from(int oldfd, unsigned long minfd, int cloexec)
{
	struct files_struct *files = current_files();
	struct file *file = fd_get(oldfd);
	int newfd = -EMFILE;

	if (!file)
		return -EBADF;
	if (!files) {
		file_put(file);
		return -EBADF;
	}
	if (minfd >= NR_OPEN) {
		file_put(file);
		return -EINVAL;
	}

	mutex_lock(&files->lock);
	for (int fd = (int)minfd; fd < NR_OPEN; fd++) {
		if (!files->fd[fd]) {
			file_get(file);
			files->fd[fd] = file;
			if (cloexec)
				files->close_on_exec |=
					(1UL << (unsigned int)fd);
			else
				files->close_on_exec &=
					~(1UL << (unsigned int)fd);
			newfd = fd;
			break;
		}
	}
	mutex_unlock(&files->lock);

	file_put(file);
	return newfd;
}

int fd_dup2(int oldfd, int newfd, int cloexec)
{
	struct files_struct *files = current_files();
	struct file *file = fd_get(oldfd);
	struct file *old = NULL;

	if (!file)
		return -EBADF;
	if (!files || newfd < 0 || newfd >= NR_OPEN) {
		file_put(file);
		return -EBADF;
	}
	if (oldfd == newfd) {
		file_put(file);
		return newfd;
	}

	mutex_lock(&files->lock);
	old = files->fd[newfd];
	file_get(file);
	files->fd[newfd] = file;
	if (cloexec)
		files->close_on_exec |= (1UL << (unsigned int)newfd);
	else
		files->close_on_exec &= ~(1UL << (unsigned int)newfd);
	mutex_unlock(&files->lock);

	file_put(old);
	file_put(file);

	return newfd;
}

/*
 * fs/vfs/file.c - Stage 4 最小 file/fd 层
 */

#include <nuvix/fdtable.h>
#include <nuvix/slab.h>
#include <nuvix/page_cache.h>
#include <nuvix/printk.h>
#include <nuvix/stat.h>
#include <nuvix/statfs.h>
#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/vfs.h>

#define FILE_STATUS_FLAGS	 (O_ACCMODE | O_APPEND | O_NONBLOCK | O_DIRECTORY)
#define FILE_SETFL_MUTABLE_FLAGS (O_APPEND | O_NONBLOCK)
#define FILE_SETFL_UNSUPPORTED_FLAGS                                           \
	(O_DSYNC | FASYNC | O_DIRECT | O_NOATIME | __O_SYNC)

static ssize_t null_read(struct file *file, char *buf, size_t count, loff_t pos);
static ssize_t null_write(struct file *file, const char *buf, size_t count, loff_t pos);
static int null_poll(struct file *file, uint32_t events, struct task_wait *wait);

#define VFS_CHRDEV_MAX 8

struct vfs_chrdev {
	dev_t dev;
	const struct file_operations *fops;
};

static const struct file_operations null_fops = {
	.read = null_read,
	.write = null_write,
	.poll = null_poll,
};

static struct vfs_chrdev vfs_chrdevs[VFS_CHRDEV_MAX] = {
	{.dev = MKDEV(1, 3), .fops = &null_fops},
};

int vfs_register_chrdev(dev_t dev, const struct file_operations *fops)
{
	int free_slot = -1;

	if (!fops)
		return -EINVAL;

	for (int i = 0; i < VFS_CHRDEV_MAX; i++) {
		if (vfs_chrdevs[i].fops && vfs_chrdevs[i].dev == dev)
			return -EEXIST;
		if (!vfs_chrdevs[i].fops && free_slot < 0)
			free_slot = i;
	}

	if (free_slot < 0)
		return -ENOMEM;

	vfs_chrdevs[free_slot].dev = dev;
	vfs_chrdevs[free_slot].fops = fops;
	return 0;
}

const struct file_operations *vfs_chrdev_fops(dev_t dev)
{
	for (int i = 0; i < VFS_CHRDEV_MAX; i++) {
		if (vfs_chrdevs[i].fops && vfs_chrdevs[i].dev == dev)
			return vfs_chrdevs[i].fops;
	}

	return NULL;
}

int vfs_sync_file(struct file *file)
{
	int ret;

	if (!file || !file->f_inode)
		return -EINVAL;

	ret = pgcache_sync_inode(file->f_inode);
	if (ret < 0)
		return ret;

	return vfs_inode_writeback(file->f_inode);
}

int vfs_msync_file_range(struct file *file, uint64_t first_page,
			 uint64_t end_page)
{
	int ret;

	if (!file || !file->f_inode)
		return -EINVAL;

	ret = pgcache_msync_mapping_range(&file->f_inode->i_pages, first_page,
					 end_page);
	if (ret < 0)
		return ret;

	return vfs_inode_writeback(file->f_inode);
}

int vfs_datasync_file(struct file *file)
{
	int ret;

	if (!file || !file->f_inode)
		return -EINVAL;

	ret = pgcache_sync_inode(file->f_inode);
	if (ret < 0)
		return ret;

	return vfs_inode_datasync(file->f_inode);
}

int vfs_truncate_file(struct file *file, uint64_t size)
{
	if (!file || !file->f_inode)
		return -EINVAL;

	return vfs_inode_truncate(file->f_inode, size);
}

int vfs_fallocate_file(struct file *file, int mode, uint64_t offset,
		       uint64_t len)
{
	struct inode *inode = file ? file->f_inode : NULL;

	if (!inode)
		return -EINVAL;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;
	if (!inode->i_op || !inode->i_op->fallocate)
		return -EINVAL;

	return inode->i_op->fallocate(inode, mode, offset, len);
}

int vfs_stat_file(struct file *file, struct stat *st)
{
	if (!file)
		return -EINVAL;

	return vfs_stat_inode(file->f_inode, st);
}

int vfs_statfs(struct super_block *sb, struct statfs64 *buf)
{
	if (!sb || !buf)
		return -EINVAL;
	if (!sb->s_op || !sb->s_op->statfs)
		return -ENOSYS;

	return sb->s_op->statfs(sb, buf);
}

int vfs_poll(struct file *file, uint32_t events,
	     struct task_wait *wait)
{
	uint32_t mask = 0;

	if (!file)
		return POLLNVAL;
	if (file->f_op && file->f_op->poll)
		return file->f_op->poll(file, events, wait);

	if ((events & POLLIN) && (file->f_mode & FMODE_READ))
		mask |= POLLIN;
	if ((events & POLLOUT) && (file->f_mode & FMODE_WRITE))
		mask |= POLLOUT;
	return mask;
}

int vfs_ioctl(struct file *file, uint64_t cmd, uint64_t arg)
{
	if (!file)
		return -EINVAL;
	if (!file->f_op || !file->f_op->ioctl)
		return -ENOTTY;

	return file->f_op->ioctl(file, cmd, arg);
}

int file_get_status_flags(struct file *file)
{
	if (!file)
		return -EBADF;

	return (int)(file->f_flags & FILE_STATUS_FLAGS);
}

int file_set_status_flags(struct file *file, uint32_t flags)
{
	uint32_t old_flags;

	if (!file)
		return -EBADF;
	if (flags & FILE_SETFL_UNSUPPORTED_FLAGS)
		return -EINVAL;

	old_flags = file->f_flags;
	file->f_flags = (old_flags & ~FILE_SETFL_MUTABLE_FLAGS) |
			(flags & FILE_SETFL_MUTABLE_FLAGS);

	return 0;
}

struct file *file_alloc(const struct file_operations *f_op, uint32_t mode,
			void *private_data)
{
	struct file *file = kmalloc(sizeof(*file), ALLOC_NOWAIT);
	if (!file)
		return NULL;

	memset(file, 0, sizeof(*file));
	file->f_op = f_op;
	file->f_mode = mode;
	file->private_data = private_data;
	refcount_set(&file->refcount, 1);

	return file;
}

struct file *file_alloc_path(const struct path *path, uint32_t flags,
			     uint32_t mode)
{
	const struct file_operations *f_op;
	struct file *file;
	struct dentry *dentry;

	if (!path || !path->mnt || !path->dentry || !path->dentry->d_inode)
		return NULL;

	dentry = path->dentry;
	f_op = dentry->d_inode->i_fop;

	if ((dentry->d_inode->i_mode & S_IFMT) == S_IFCHR) {
		f_op = vfs_chrdev_fops(dentry->d_inode->i_rdev);
		if (!f_op)
			return NULL;
	}

	file = file_alloc(f_op, mode, NULL);
	if (!file)
		return NULL;

	file->f_path = *path;
	path_get(&file->f_path);
	file->f_inode = dentry->d_inode;
	file->f_flags = flags;
	igrab(file->f_inode);

	if (file->f_op && file->f_op->open) {
		int ret = file->f_op->open(file->f_inode, file);
		if (ret < 0) {
			file->f_inode = NULL;
			path_put(&file->f_path);
			iput(dentry->d_inode);
			kfree(file);
			return NULL;
		}
	}

	return file;
}

int vfs_openat_path(const struct path *base, const char *path, uint32_t flags,
		    uint32_t mode)
{
	struct dentry *dentry;
	struct path found_path;
	struct file *file;
	int fd;
	int ret;
	uint32_t fmode;

	ret = path_lookupat_path(base, path, 0, &found_path);
	if (ret < 0) {
		if (!(flags & O_CREAT) || ret != -ENOENT)
			return ret;

		ret = vfs_create_at_path(base, path, mode, &found_path);
		if (ret < 0)
			return ret;
	} else if ((flags & O_CREAT) && (flags & O_EXCL)) {
		path_put(&found_path);
		return -EEXIST;
	}

	dentry = found_path.dentry;

	if ((flags & O_DIRECTORY) && dentry->d_inode &&
	    (dentry->d_inode->i_mode & S_IFMT) != S_IFDIR) {
		path_put(&found_path);
		return -ENOTDIR;
	}

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		fmode = FMODE_READ;
		break;
	case O_WRONLY:
		fmode = FMODE_WRITE;
		break;
	case O_RDWR:
		fmode = FMODE_READ | FMODE_WRITE;
		break;
	default:
		path_put(&found_path);
		return -EINVAL;
	}

	ret = 0;
	if (fmode & FMODE_READ)
		ret = vfs_inode_permission(dentry->d_inode, VFS_MAY_READ);
	if (ret == 0 && (fmode & FMODE_WRITE))
		ret = vfs_inode_permission(dentry->d_inode, VFS_MAY_WRITE);
	if (ret < 0) {
		path_put(&found_path);
		return ret;
	}

	file = file_alloc_path(&found_path, flags, fmode);
	path_put(&found_path);
	if (!file)
		return -ENOMEM;

	if ((flags & O_TRUNC) && (fmode & FMODE_WRITE) && file->f_inode) {
		ret = vfs_truncate_file(file, 0);
		if (ret < 0) {
			file_put(file);
			return ret;
		}
	}

	fd = fd_alloc_flags(file, flags);
	if (fd < 0) {
		file_put(file);
		return fd;
	}

	return fd;
}

int vfs_openat(struct dentry *base, const char *path, uint32_t flags,
	       uint32_t mode)
{
	struct path base_path;
	int ret;

	if (!base)
		return vfs_openat_path(NULL, path, flags, mode);

	ret = vfs_path_from_dentry(base, &base_path);
	if (ret < 0)
		return ret;

	ret = vfs_openat_path(&base_path, path, flags, mode);
	path_put(&base_path);
	return ret;
}

int vfs_open(const char *path, uint32_t flags, uint32_t mode)
{
	return vfs_openat_path(NULL, path, flags, mode);
}

void file_get(struct file *file)
{
	if (file && !file->static_file)
		refcount_inc(&file->refcount);
}

void file_put(struct file *file)
{
	if (!file || file->static_file)
		return;

	if (!refcount_dec_and_test(&file->refcount))
		return;

	if (file->f_op && file->f_op->release)
		file->f_op->release(file);

	path_put(&file->f_path);
	iput(file->f_inode);
	kfree(file);
}

static ssize_t null_read(struct file *file, char *buf, size_t count, loff_t pos)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)pos;

	return 0;
}

static ssize_t null_write(struct file *file, const char *buf, size_t count,
			  loff_t pos)
{
	(void)file;
	(void)buf;
	(void)pos;

	return (ssize_t)count;
}

static int null_poll(struct file *file, uint32_t events,
		     struct task_wait *wait)
{
	uint32_t mask = 0;

	(void)wait;

	if ((events & POLLIN) && (file->f_mode & FMODE_READ))
		mask |= POLLIN;
	if ((events & POLLOUT) && (file->f_mode & FMODE_WRITE))
		mask |= POLLOUT;
	return mask;
}

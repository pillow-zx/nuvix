/*
 * fs/vfs/fs_struct.c - 可共享的进程文件系统上下文
 */

#include <kernel/errno.h>
#include <kernel/fs_struct.h>
#include <kernel/proc.h>
#include <kernel/slab.h>
#include <kernel/vfs.h>

static void fs_set_initial_root(struct fs_struct *fs)
{
	struct path root = {0};

	if (!fs || !root_dentry)
		return;
	if (vfs_root_path(&root) < 0)
		return;

	fs->root = root;
	fs->cwd = root;
	path_get(&fs->cwd);
}

struct fs_struct *fs_alloc(void)
{
	struct fs_struct *fs = kmalloc(sizeof(*fs), ALLOC_NOWAIT);

	if (!fs)
		return NULL;

	memset(fs, 0, sizeof(*fs));
	refcount_set(&fs->refcount, 1);
	mutex_init(&fs->lock);
	fs->umask = 0022;
	fs_set_initial_root(fs);
	return fs;
}

struct fs_struct *fs_dup(struct fs_struct *old)
{
	struct fs_struct *fs = fs_alloc();

	if (!fs)
		return NULL;
	if (!old)
		return fs;

	path_put(&fs->root);
	path_put(&fs->cwd);

	mutex_lock(&old->lock);
	fs->umask = old->umask;
	fs->root = old->root;
	fs->cwd = old->cwd;
	path_get(&fs->root);
	path_get(&fs->cwd);
	mutex_unlock(&old->lock);

	return fs;
}

void fs_get(struct fs_struct *fs)
{
	if (fs)
		refcount_inc(&fs->refcount);
}

void fs_put(struct fs_struct *fs)
{
	if (!fs)
		return;

	if (!refcount_dec_and_test(&fs->refcount))
		return;

	path_put(&fs->root);
	path_put(&fs->cwd);
	kfree(fs);
}

int fs_get_root_path(struct fs_struct *fs, struct path *path)
{
	if (!path)
		return -EINVAL;
	path->mnt = NULL;
	path->dentry = NULL;

	if (fs) {
		mutex_lock(&fs->lock);
		*path = fs->root;
		path_get(path);
		mutex_unlock(&fs->lock);
	}

	if (!path->dentry)
		return vfs_root_path(path);

	return path->dentry ? 0 : -ENOENT;
}

int fs_get_cwd_path(struct fs_struct *fs, struct path *path)
{
	if (!path)
		return -EINVAL;
	path->mnt = NULL;
	path->dentry = NULL;

	if (fs) {
		mutex_lock(&fs->lock);
		*path = fs->cwd.dentry ? fs->cwd : fs->root;
		path_get(path);
		mutex_unlock(&fs->lock);
	}

	if (!path->dentry)
		return vfs_root_path(path);

	return path->dentry ? 0 : -ENOENT;
}

int fs_set_cwd_path(struct fs_struct *fs, const struct path *path)
{
	struct path old;

	if (!fs || !path || !path->dentry)
		return -EINVAL;

	path_get(path);
	mutex_lock(&fs->lock);
	old = fs->cwd;
	fs->cwd = *path;
	mutex_unlock(&fs->lock);

	path_put(&old);
	return 0;
}

uint32_t fs_get_umask(struct fs_struct *fs)
{
	uint32_t umask = 0022;

	if (!fs)
		return umask;

	mutex_lock(&fs->lock);
	umask = fs->umask;
	mutex_unlock(&fs->lock);
	return umask;
}

uint32_t fs_set_umask(struct fs_struct *fs, uint32_t mask)
{
	uint32_t old = 0022;

	if (!fs)
		return old;

	mutex_lock(&fs->lock);
	old = fs->umask;
	fs->umask = mask;
	mutex_unlock(&fs->lock);
	return old;
}

void fs_set_root_if_empty(struct fs_struct *fs, struct dentry *root)
{
	struct path path;

	if (!fs || !root)
		return;
	if (vfs_path_from_dentry(root, &path) < 0)
		return;

	mutex_lock(&fs->lock);
	if (!fs->root.dentry) {
		fs->root = path;
		path_get(&fs->root);
	}
	if (!fs->cwd.dentry) {
		fs->cwd = path;
		path_get(&fs->cwd);
	}
	mutex_unlock(&fs->lock);
	path_put(&path);
}

int init_fs(struct proc_struct *proc)
{
	struct fs_struct *fs;
	struct fs_struct *old;

	if (!proc)
		return -EINVAL;

	fs = fs_alloc();
	if (!fs)
		return -ENOMEM;
	old = proc_replace_fs(proc, fs);
	fs_put(old);
	return 0;
}

int copy_fs(const struct proc_struct *source, struct proc_struct *dest,
		   bool share)
{
	struct fs_struct *fs;
	struct fs_struct *old;

	if (!dest)
		return -EINVAL;

	if (share) {
		fs = source ? source->fs : NULL;
		if (!fs)
			return init_fs(dest);
		fs_get(fs);
	} else {
		fs = fs_dup(source ? source->fs : NULL);
		if (!fs)
			return -ENOMEM;
	}

	old = proc_replace_fs(dest, fs);
	fs_put(old);
	return 0;
}

void exit_fs(struct proc_struct *proc)
{
	struct fs_struct *fs;

	if (!proc)
		return;

	fs = proc_replace_fs(proc, NULL);
	fs_put(fs);
}

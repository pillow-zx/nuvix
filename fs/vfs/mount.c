/*
 * fs/vfs/mount.c - single-namespace VFS mount table
 */

#include <nuvix/blkdev.h>
#include <nuvix/bootinfo.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/page_cache.h>
#include <nuvix/slab.h>
#include <nuvix/stat.h>
#include <nuvix/mutex.h>
#include <nuvix/printk.h>
#include <nuvix/vfs.h>

#include "namei_internal.h"

struct mount_device_claim {
	dev_t dev;
	struct list_head node;
};

static LIST_HEAD(mount_list);
static LIST_HEAD(mount_device_claims);
static DEFINE_MUTEX(mount_lock, LOCK_RANK_VFS_MOUNT, LOCK_IRQ_TASK_ONLY);
static struct vfsmount *root_mount;

int vfs_sync_all(void)
{
	int first_error;
	struct vfsmount **mounts = NULL;
	struct vfsmount *mnt;
	size_t count = 0;
	size_t index = 0;

	first_error = pgcache_sync_all();
	mutex_lock(&mount_lock);
	list_for_each_entry (mnt, &mount_list, mnt_list)
		count++;
	if (count != 0) {
		mounts = kmalloc_array(count, sizeof(*mounts), ALLOC_NOWAIT);
		if (!mounts) {
			mutex_unlock(&mount_lock);
			return first_error < 0 ? first_error : -ENOMEM;
		}
		list_for_each_entry (mnt, &mount_list, mnt_list) {
			mntget(mnt);
			mounts[index++] = mnt;
		}
	}
	mutex_unlock(&mount_lock);

	for (index = 0; index < count; index++) {
		const struct super_operations *ops =
			mounts[index]->mnt_sb->s_op;
		int ret = 0;

		if (ops && ops->sync_fs)
			ret = ops->sync_fs(mounts[index]->mnt_sb);
		if (ret < 0 && first_error == 0)
			first_error = ret;
		mntput(mounts[index]);
	}
	kfree(mounts);

	return first_error;
}

static int vfs_sync_mount(struct vfsmount *mnt)
{
	const struct super_operations *ops;
	int ret;

	if (!mnt || !mnt->mnt_sb)
		return -EINVAL;
	ret = pgcache_sync_device(mnt->mnt_dev);
	if (ret < 0)
		return ret;
	ops = mnt->mnt_sb->s_op;
	if (ops && ops->sync_fs) {
		ret = ops->sync_fs(mnt->mnt_sb);
		if (ret < 0)
			return ret;
	}
	return mnt->mnt_sb->s_error;
}

static struct vfsmount *mount_find_by_mountpoint(const struct path *mountpoint)
{
	struct vfsmount *mnt;

	if (!mountpoint || !mountpoint->mnt || !mountpoint->dentry)
		return NULL;

	list_for_each_entry (mnt, &mount_list, mnt_list) {
		if (mnt->mnt_parent == mountpoint->mnt &&
		    mnt->mnt_mountpoint == mountpoint->dentry)
			return mnt;
	}

	return NULL;
}

static struct vfsmount *mount_find_by_root(const struct path *root)
{
	struct vfsmount *mnt;

	if (!root || !root->mnt || !root->dentry)
		return NULL;

	list_for_each_entry (mnt, &mount_list, mnt_list) {
		if (mnt == root->mnt && mnt->mnt_root == root->dentry)
			return mnt;
	}

	return NULL;
}

static struct vfsmount *mount_find_by_sb(struct super_block *sb)
{
	struct vfsmount *mnt;

	if (!sb)
		return NULL;

	list_for_each_entry (mnt, &mount_list, mnt_list) {
		if (mnt->mnt_sb == sb)
			return mnt;
	}

	return NULL;
}

static struct vfsmount *mount_find_by_dev(dev_t dev)
{
	struct vfsmount *mnt;

	list_for_each_entry (mnt, &mount_list, mnt_list) {
		if (mnt->mnt_dev == dev)
			return mnt;
	}
	return NULL;
}

static bool mount_device_claimed_locked(dev_t dev)
{
	struct mount_device_claim *claim;

	if (mount_find_by_dev(dev))
		return true;
	list_for_each_entry (claim, &mount_device_claims, node) {
		if (claim->dev == dev)
			return true;
	}
	return false;
}

void mntget(struct vfsmount *mnt)
{
	if (mnt)
		refcount_inc(&mnt->mnt_refcount);
}

static void mnt_active_get(struct vfsmount *mnt)
{
	if (mnt)
		atomic_inc(&mnt->mnt_active_refs);
}

static void mnt_active_put(struct vfsmount *mnt)
{
	if (mnt)
		atomic_dec(&mnt->mnt_active_refs);
}

static void mount_free(struct vfsmount *mnt)
{
	struct super_block *sb;
	bool destroy_sb;

	if (!mnt)
		return;
	sb = mnt->mnt_sb;
	destroy_sb = mnt->mnt_state == MNT_DETACHED;
	mnt->mnt_state = MNT_DEAD;

	if (mnt->mnt_parent)
		mntput(mnt->mnt_parent);
	dput(mnt->mnt_root);
	dput(mnt->mnt_mountpoint);
	mnt->mnt_sb = NULL;
	kfree(mnt);
	if (destroy_sb)
		vfs_super_destroy(sb);
}

void mntput(struct vfsmount *mnt)
{
	if (!mnt)
		return;

	if (refcount_dec_and_test(&mnt->mnt_refcount))
		mount_free(mnt);
}

void path_get(const struct path *path)
{
	if (!path)
		return;
	mntget(path->mnt);
	mnt_active_get(path->mnt);
	dget(path->dentry);
}

void path_put(struct path *path)
{
	if (!path)
		return;
	dput(path->dentry);
	mnt_active_put(path->mnt);
	mntput(path->mnt);
	path->mnt = NULL;
	path->dentry = NULL;
}

int vfs_root_path(struct path *path)
{
	if (!path)
		return -EINVAL;

	mutex_lock(&mount_lock);
	if (!root_mount) {
		mutex_unlock(&mount_lock);
		path->mnt = NULL;
		path->dentry = NULL;
		return -ENOENT;
	}
	path->mnt = root_mount;
	path->dentry = root_mount->mnt_root;
	path_get(path);
	mutex_unlock(&mount_lock);
	return 0;
}

int vfs_path_from_dentry(struct dentry *dentry, struct path *path)
{
	struct vfsmount *mnt;

	if (path) {
		path->mnt = NULL;
		path->dentry = NULL;
	}
	if (!dentry || !path)
		return -EINVAL;

	mutex_lock(&mount_lock);
	mnt = mount_find_by_sb(dentry->d_sb);
	if (!mnt || mnt->mnt_state != MNT_ATTACHED) {
		mutex_unlock(&mount_lock);
		return -ENOENT;
	}
	path->mnt = mnt;
	path->dentry = dentry;
	path_get(path);
	mutex_unlock(&mount_lock);
	return 0;
}

static int mount_add(const struct path *mountpoint, struct dentry *root,
		     struct super_block *sb, dev_t dev, bool is_root)
{
	struct vfsmount *mnt;

	if (!mountpoint || !mountpoint->dentry || !root || !sb)
		return -EINVAL;

	mnt = kmalloc(sizeof(*mnt), ALLOC_NOWAIT);
	if (!mnt)
		return -ENOMEM;

	refcount_set(&mnt->mnt_refcount, 1);
	atomic_set(&mnt->mnt_active_refs, 0);
	mnt->mnt_parent = is_root ? NULL : mountpoint->mnt;
	mnt->mnt_mountpoint = mountpoint->dentry;
	mnt->mnt_root = root;
	mnt->mnt_sb = sb;
	mnt->mnt_dev = dev;
	mnt->mnt_is_root = is_root;
	mnt->mnt_state = MNT_UNPUBLISHED;
	INIT_LIST_HEAD(&mnt->mnt_list);
	if (mnt->mnt_parent)
		mntget(mnt->mnt_parent);
	dget(mnt->mnt_mountpoint);
	dget(root);

	mutex_lock(&mount_lock);
	if ((is_root && root_mount) ||
	    (!is_root && mount_find_by_mountpoint(mountpoint))) {
		mutex_unlock(&mount_lock);
		mount_free(mnt);
		return -EBUSY;
	}
	list_add_tail(&mnt->mnt_list, &mount_list);
	mnt->mnt_state = MNT_ATTACHED;
	if (is_root)
		root_mount = mnt;
	mutex_unlock(&mount_lock);

	return 0;
}

static int mount_attach(const struct path *mountpoint,
			struct file_system_type *fs_type, dev_t dev,
			bool is_root, const void *data)
{
	struct super_block *sb = NULL;
	struct path attach_point;
	struct mount_device_claim *claim;
	int ret;

	if (!mountpoint || !fs_type || !fs_type->mount)
		return -EINVAL;

	claim = kmalloc(sizeof(*claim), ALLOC_NOWAIT);
	if (!claim)
		return -ENOMEM;
	claim->dev = dev;
	INIT_LIST_HEAD(&claim->node);
	mutex_lock(&mount_lock);
	if (mount_device_claimed_locked(dev)) {
		mutex_unlock(&mount_lock);
		kfree(claim);
		return -EBUSY;
	}
	list_add_tail(&claim->node, &mount_device_claims);
	mutex_unlock(&mount_lock);

	ret = fs_type->mount(fs_type, dev, data, &sb);
	if (ret < 0)
		goto out_claim;
	if (!sb || !sb->s_root) {
		ret = -EINVAL;
		goto out_destroy;
	}

	attach_point = *mountpoint;
	if (is_root && !attach_point.dentry)
		attach_point.dentry = sb->s_root;

	ret = mount_add(&attach_point, sb->s_root, sb, dev, is_root);
	if (ret < 0)
		goto out_destroy;
	if (is_root)
		vfs_set_root_dentry(sb->s_root);
	ret = 0;
	goto out_claim;

out_destroy:
	vfs_super_destroy(sb);
out_claim:
	mutex_lock(&mount_lock);
	list_del_init(&claim->node);
	mutex_unlock(&mount_lock);
	kfree(claim);
	return ret;
}

static int vfs_select_rootfs(dev_t dev, struct file_system_type **out_fs)
{
	struct file_system_type *fs_type;
	struct file_system_type *match = NULL;
	int ret;

	if (!out_fs)
		return -EINVAL;
	*out_fs = NULL;

	if (!lookup_blkdev(dev))
		return -ENXIO;

	for (fs_type = get_next_filesystem_type(NULL); fs_type;
	     fs_type = get_next_filesystem_type(fs_type)) {
		if (!fs_type->probe)
			continue;

		ret = fs_type->probe(dev);
		if (ret < 0) {
			pr_err("VFS: %s root probe failed (%d)\n",
			       fs_type->name, ret);
			return ret;
		}
		if (ret == 0)
			continue;

		if (match) {
			pr_err("VFS: ambiguous root filesystem match: %s, %s\n",
			       match->name, fs_type->name);
			return -EINVAL;
		}
		match = fs_type;
	}

	if (!match)
		return -ENODEV;

	*out_fs = match;
	return 0;
}

int vfs_mount_root(dev_t dev)
{
	struct file_system_type *fs_type = NULL;
	struct path root_path = {0};
	int ret;

	ret = vfs_select_rootfs(dev, &fs_type);
	if (ret < 0) {
		pr_err("VFS: no root filesystem for dev %u:%u (%d)\n",
		       MAJOR(dev), MINOR(dev), ret);
		return ret;
	}

	root_path.dentry = NULL;
	ret = mount_attach(&root_path, fs_type, dev, true, NULL);
	if (ret < 0) {
		pr_err("VFS: failed to mount root as %s on dev %u:%u (%d)\n",
		       fs_type->name, MAJOR(dev), MINOR(dev), ret);
		return ret;
	}

	bootinfo_block(fs_type->name);
	return 0;
}

int vfs_follow_mount(struct path *path)
{
	struct vfsmount *mnt;
	struct path next;

	if (!path || !path->mnt || !path->dentry)
		return -EINVAL;

	mutex_lock(&mount_lock);
	mnt = mount_find_by_mountpoint(path);
	if (mnt && (mnt != path->mnt || mnt->mnt_root != path->dentry)) {
		if (mnt->mnt_state != MNT_ATTACHED) {
			mutex_unlock(&mount_lock);
			return -EBUSY;
		}
		next.mnt = mnt;
		next.dentry = mnt->mnt_root;
		path_get(&next);
		mutex_unlock(&mount_lock);
		path_put(path);
		*path = next;
		return 0;
	}
	mutex_unlock(&mount_lock);
	return 0;
}

int vfs_follow_dotdot_mount(struct path *path)
{
	struct vfsmount *mnt;
	struct path parent;

	if (!path || !path->mnt || !path->dentry)
		return -EINVAL;

	mutex_lock(&mount_lock);
	mnt = mount_find_by_root(path);
	if (mnt && !mnt->mnt_is_root) {
		parent.mnt = mnt->mnt_parent;
		parent.dentry = mnt->mnt_mountpoint;
		path_get(&parent);
		mutex_unlock(&mount_lock);
		path_put(path);
		*path = parent;
		return 0;
	}
	mutex_unlock(&mount_lock);
	return 0;
}

int vfs_mount(const char *source, const char *target, const char *type,
	      unsigned long flags, const void *data)
{
	struct path source_path = {0};
	struct path target_path = {0};
	struct file_system_type *fs_type;
	struct inode *source_inode;
	dev_t dev;
	int ret;

	if (!source || !target || !type)
		return -EFAULT;
	if (flags)
		return -EINVAL;

	fs_type = get_filesystem_type(type);
	if (!fs_type || !fs_type->mount)
		return -ENODEV;

	ret = path_lookupat_path(NULL, source, 0, &source_path);
	if (ret < 0)
		return ret;
	source_inode = source_path.dentry->d_inode;
	if (!source_inode || !S_ISBLK(source_inode->i_mode)) {
		path_put(&source_path);
		return -ENOTBLK;
	}
	dev = source_inode->i_rdev;
	path_put(&source_path);

	if (!lookup_blkdev(dev))
		return -ENXIO;

	ret = path_lookupat_path(NULL, target, LOOKUP_NO_MOUNT, &target_path);
	if (ret < 0)
		return ret;
	if (!target_path.dentry->d_inode ||
	    !S_ISDIR(target_path.dentry->d_inode->i_mode)) {
		path_put(&target_path);
		return -ENOTDIR;
	}

	mutex_lock(&mount_lock);
	if (mount_find_by_mountpoint(&target_path)) {
		mutex_unlock(&mount_lock);
		path_put(&target_path);
		return -EBUSY;
	}
	mutex_unlock(&mount_lock);

	ret = mount_attach(&target_path, fs_type, dev, false, data);
	path_put(&target_path);
	return ret;
}

static bool mount_busy_locked(struct vfsmount *mnt)
{
	struct vfsmount *child;

	if (!mnt || !mnt->mnt_root)
		return true;

	if (atomic_read(&mnt->mnt_active_refs) > 0)
		return true;
	list_for_each_entry (child, &mount_list, mnt_list) {
		if (child != mnt && child->mnt_parent == mnt &&
		    child->mnt_state != MNT_DEAD)
			return true;
	}
	return false;
}

int vfs_umount(const char *target, int flags)
{
	struct path path = {0};
	struct vfsmount *mnt;
	int ret;

	if (!target)
		return -EFAULT;
	if (flags)
		return -EINVAL;

	ret = path_lookupat_path(NULL, target, LOOKUP_NOFOLLOW, &path);
	if (ret < 0)
		return ret;

	mutex_lock(&mount_lock);
	mnt = mount_find_by_root(&path);
	if (!mnt) {
		mutex_unlock(&mount_lock);
		path_put(&path);
		return -EINVAL;
	}
	mntget(mnt);
	mutex_unlock(&mount_lock);
	path_put(&path);

	mutex_lock(&mount_lock);
	if (mnt->mnt_is_root || mnt->mnt_state != MNT_ATTACHED ||
	    mount_busy_locked(mnt)) {
		mutex_unlock(&mount_lock);
		mntput(mnt);
		return -EBUSY;
	}
	mnt->mnt_state = MNT_QUIESCING;
	mutex_unlock(&mount_lock);

	ret = vfs_sync_mount(mnt);
	if (ret < 0) {
		mutex_lock(&mount_lock);
		if (mnt->mnt_state == MNT_QUIESCING)
			mnt->mnt_state = MNT_ATTACHED;
		mutex_unlock(&mount_lock);
		mntput(mnt);
		return ret;
	}

	mutex_lock(&mount_lock);
	if (mnt->mnt_state != MNT_QUIESCING || mount_busy_locked(mnt)) {
		mnt->mnt_state = MNT_ATTACHED;
		mutex_unlock(&mount_lock);
		mntput(mnt);
		return -EBUSY;
	}
	/* Hold the table lock across the teardown transaction: the mount is
	 * quiesced and not busily referenced, so the failable dentry/inode
	 * retirement sees a stable namespace view; nothing else can publish
	 * a path into this subtree between the busy check and DETACHED. */
	ret = vfs_super_teardown(mnt->mnt_sb);
	if (ret < 0)
		goto out_restore;
	ret = pgcache_discard_device(mnt->mnt_dev);
	if (ret < 0) {
		vfs_super_mark_aborted(mnt->mnt_sb);
		goto out_restore;
	}
	mnt->mnt_state = MNT_DETACHED;
	list_del_init(&mnt->mnt_list);
	mutex_unlock(&mount_lock);

	mntput(mnt);
	mntput(mnt);
	return 0;

out_restore:
	mnt->mnt_state = MNT_ATTACHED;
	mutex_unlock(&mount_lock);
	mntput(mnt);
	return ret;
}

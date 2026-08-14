/*
 * fs/vfs/namei_mutation.c - VFS 命名修改操作
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/stat.h>
#include <nuvix/vfs.h>

#include "namei_internal.h"

struct create_target {
	struct path parent;
	struct dentry *dentry;
	bool new_dentry;
};

static int prepare_create_target(const struct path *base, const char *path,
				 bool require_mkdir,
				 struct create_target *target)
{
	char name[VFS_NAME_MAX + 1];
	size_t namelen;
	struct dentry *parent;
	struct dentry *dentry;
	int ret;

	target->parent.mnt = NULL;
	target->parent.dentry = NULL;
	target->dentry = NULL;
	target->new_dentry = false;

	ret = path_parent_lookupat_path(base, path, name, &namelen,
					&target->parent);
	if (ret < 0)
		return ret;
	parent = target->parent.dentry;
	if (!parent->d_inode || !parent->d_inode->i_op ||
	    (require_mkdir && !parent->d_inode->i_op->mkdir) ||
	    (!require_mkdir && !parent->d_inode->i_op->create)) {
		path_put(&target->parent);
		return -ENOTDIR;
	}

	dentry = dcache_lookup(parent, name, namelen);
	if (dentry && dentry->d_inode) {
		dput(dentry);
		path_put(&target->parent);
		return -EEXIST;
	}
	if (!dentry) {
		dentry = dentry_alloc(parent, name, namelen);
		if (!dentry) {
			path_put(&target->parent);
			return -ENOMEM;
		}
		target->new_dentry = true;
	}

	target->dentry = dentry;
	return 0;
}

static void put_create_target(struct create_target *target)
{
	if (target->dentry)
		dput(target->dentry);
	path_put(&target->parent);
}

static void rollback_create_target(struct create_target *target, bool directory)
{
	struct dentry *parent = target->parent.dentry;
	int ret;

	if (!parent || !parent->d_inode || !parent->d_inode->i_op ||
	    !target->dentry ||
	    !target->dentry->d_inode)
		return;

	if (directory) {
		if (!parent->d_inode->i_op->rmdir)
			return;
		ret = parent->d_inode->i_op->rmdir(parent->d_inode,
						   target->dentry);
	} else {
		if (!parent->d_inode->i_op->unlink)
			return;
		ret = parent->d_inode->i_op->unlink(parent->d_inode,
						    target->dentry);
	}

	if (ret == 0)
		dcache_invalidate(target->dentry);
}

int vfs_create_at_path(const struct path *base, const char *path, uint32_t mode,
		       struct path *res)
{
	struct create_target target;
	struct dentry *parent;
	int ret;

	if (res) {
		res->mnt = NULL;
		res->dentry = NULL;
	}

	ret = prepare_create_target(base, path, false, &target);
	if (ret < 0)
		return ret;

	parent = target.parent.dentry;
	ret = parent->d_inode->i_op->create(parent->d_inode, target.dentry,
					    mode);
	if (ret == 0) {
		ret = vfs_init_inode_owner(target.dentry->d_inode);
		if (ret < 0) {
			rollback_create_target(&target, false);
		} else {
			if (target.new_dentry)
				dcache_insert(target.dentry);
			if (res) {
				res->mnt = target.parent.mnt;
				res->dentry = target.dentry;
				path_get(res);
			}
		}
	}

	put_create_target(&target);
	return ret;
}

int vfs_symlink_at_path(const struct path *base, const char *target,
			const char *linkpath)
{
	struct create_target create;
	struct dentry *parent;
	int ret;

	if (!target || !*target)
		return -ENOENT;

	ret = prepare_create_target(base, linkpath, false, &create);
	if (ret < 0)
		return ret;
	parent = create.parent.dentry;
	if (!parent->d_inode->i_op->symlink) {
		put_create_target(&create);
		return -EINVAL;
	}

	ret = parent->d_inode->i_op->symlink(parent->d_inode, create.dentry,
					     target);
	if (ret == 0) {
		ret = vfs_init_inode_owner(create.dentry->d_inode);
		if (ret < 0) {
			rollback_create_target(&create, false);
		} else if (create.new_dentry) {
			dcache_insert(create.dentry);
		}
	}

	put_create_target(&create);
	return ret;
}

int vfs_link_at_path(struct dentry *old_dentry, const struct path *new_base,
		     const char *new_path)
{
	struct create_target target;
	struct dentry *parent;
	struct inode *inode;
	int ret;

	if (!old_dentry || !old_dentry->d_inode)
		return -ENOENT;

	inode = old_dentry->d_inode;
	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	ret = prepare_create_target(new_base, new_path, false, &target);
	if (ret < 0)
		return ret;
	parent = target.parent.dentry;
	if (parent->d_inode->i_sb != inode->i_sb) {
		put_create_target(&target);
		return -EXDEV;
	}
	if (!parent->d_inode->i_op->link) {
		put_create_target(&target);
		return -EINVAL;
	}

	ret = parent->d_inode->i_op->link(old_dentry, parent->d_inode,
					  target.dentry);
	if (ret == 0 && target.new_dentry)
		dcache_insert(target.dentry);

	put_create_target(&target);
	return ret;
}

int vfs_mkdir_at_path(const struct path *base, const char *path, uint32_t mode)
{
	struct create_target target;
	struct dentry *parent;
	int ret;

	ret = prepare_create_target(base, path, true, &target);
	if (ret < 0)
		return ret;

	parent = target.parent.dentry;
	ret = parent->d_inode->i_op->mkdir(parent->d_inode, target.dentry,
					   mode);
	if (ret == 0) {
		ret = vfs_init_inode_owner(target.dentry->d_inode);
		if (ret < 0) {
			rollback_create_target(&target, true);
		} else if (target.new_dentry) {
			dcache_insert(target.dentry);
		}
	}

	put_create_target(&target);
	return ret;
}

int vfs_unlink_at_path(const struct path *base, const char *path, int flags)
{
	char name[VFS_NAME_MAX + 1];
	size_t namelen;
	struct path parent_path;
	struct dentry *parent;
	struct dentry *dentry;
	int ret;

	ret = path_parent_lookupat_path(base, path, name, &namelen,
					&parent_path);
	if (ret < 0)
		return ret;
	parent = parent_path.dentry;
	if (!parent->d_inode || !parent->d_inode->i_op) {
		path_put(&parent_path);
		return -ENOTDIR;
	}

	dentry = vfs_lookup_one(parent, name, namelen);
	if (!dentry) {
		path_put(&parent_path);
		return -ENOENT;
	}

	if (flags & AT_REMOVEDIR) {
		if (!parent->d_inode->i_op->rmdir)
			ret = -ENOTDIR;
		else
			ret = parent->d_inode->i_op->rmdir(parent->d_inode,
							   dentry);
	} else {
		if (!parent->d_inode->i_op->unlink)
			ret = -EINVAL;
		else
			ret = parent->d_inode->i_op->unlink(parent->d_inode,
							    dentry);
	}

	if (ret == 0)
		dcache_invalidate(dentry);
	dput(dentry);
	path_put(&parent_path);
	return ret;
}

static bool dentry_is_ancestor(struct dentry *ancestor, struct dentry *dentry)
{
	while (dentry) {
		if (dentry == ancestor)
			return true;
		if (dentry->d_parent == dentry)
			break;
		dentry = dentry->d_parent;
	}

	return false;
}

int vfs_rename_at_path(const struct path *old_base, const char *old_path,
		       const struct path *new_base, const char *new_path,
		       unsigned int flags)
{
	char old_name[VFS_NAME_MAX + 1];
	char new_name[VFS_NAME_MAX + 1];
	size_t old_namelen, new_namelen;
	struct path old_parent_path, new_parent_path;
	struct dentry *old_parent, *new_parent;
	struct dentry *old_dentry, *new_dentry;
	int ret;

	ret = path_parent_lookupat_path(old_base, old_path, old_name,
					&old_namelen, &old_parent_path);
	if (ret < 0)
		return ret;
	old_parent = old_parent_path.dentry;
	if (!old_parent->d_inode || !old_parent->d_inode->i_op) {
		path_put(&old_parent_path);
		return -ENOTDIR;
	}

	ret = path_parent_lookupat_path(new_base, new_path, new_name,
					&new_namelen, &new_parent_path);
	if (ret < 0) {
		path_put(&old_parent_path);
		return ret;
	}
	new_parent = new_parent_path.dentry;
	if (!new_parent->d_inode || !new_parent->d_inode->i_op) {
		path_put(&old_parent_path);
		path_put(&new_parent_path);
		return -ENOTDIR;
	}
	if (old_parent->d_inode->i_sb != new_parent->d_inode->i_sb) {
		path_put(&old_parent_path);
		path_put(&new_parent_path);
		return -EXDEV;
	}

	if (!old_parent->d_inode->i_op->rename) {
		path_put(&old_parent_path);
		path_put(&new_parent_path);
		return -EINVAL;
	}

	old_dentry = vfs_lookup_one(old_parent, old_name, old_namelen);
	if (!old_dentry) {
		path_put(&old_parent_path);
		path_put(&new_parent_path);
		return -ENOENT;
	}

	new_dentry = vfs_lookup_one_any(new_parent, new_name, new_namelen);
	if (!new_dentry) {
		dput(old_dentry);
		path_put(&old_parent_path);
		path_put(&new_parent_path);
		return -ENOMEM;
	}

	if (old_dentry == new_dentry) {
		ret = (flags & RENAME_NOREPLACE) ? -EEXIST : 0;
		goto out_dput;
	}

	if (old_dentry->d_inode && S_ISDIR(old_dentry->d_inode->i_mode) &&
	    dentry_is_ancestor(old_dentry, new_parent)) {
		ret = -EINVAL;
		goto out_dput;
	}

	ret = old_parent->d_inode->i_op->rename(old_parent->d_inode, old_dentry,
						new_parent->d_inode, new_dentry,
						flags);
	if (ret == 0) {
		dcache_invalidate(new_dentry);
		dcache_move(old_dentry, new_parent, new_name, new_namelen);
	}

out_dput:
	dput(new_dentry);
	dput(old_dentry);
	path_put(&old_parent_path);
	path_put(&new_parent_path);
	return ret;
}

int vfs_mknod_at_path(const struct path *base, const char *path, uint32_t mode,
		      dev_t dev)
{
	struct path created;
	uint32_t type = mode & S_IFMT;
	int ret;

	if (type == 0)
		mode |= S_IFREG;
	else if (type != S_IFREG && type != S_IFCHR && type != S_IFBLK)
		return -EINVAL;

	ret = vfs_create_at_path(base, path, mode, &created);
	if (ret < 0)
		return ret;

	if (created.dentry->d_inode) {
		created.dentry->d_inode->i_mode = mode;
		created.dentry->d_inode->i_rdev = dev;
		ret = vfs_inode_writeback(created.dentry->d_inode);
	}
	path_put(&created);
	return ret;
}

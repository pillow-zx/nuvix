#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

/*
 * include/nuvix/vfs.h - VFS 内部声明
 */

#include <nuvix/fs.h>

struct vfs_at_lookup_result {
	struct path path;
	struct file *file;
	struct inode *inode;
	bool empty_path;
};

__must_check
struct inode *inode_alloc(struct super_block *sb, uint64_t ino);

__must_check
struct inode *iget(struct super_block *sb, uint64_t ino);
void igrab(struct inode *inode);
void iput(struct inode *inode);
void inode_forget(struct inode *inode);
void icache_init(void);

__must_check
struct dentry *dentry_alloc(struct dentry *parent,
					 const char *name, size_t namelen);

__must_check
struct dentry *dcache_lookup(struct dentry *parent,
					  const char *name, size_t namelen);
void dcache_insert(struct dentry *dentry);
void dcache_invalidate(struct dentry *dentry);
void dcache_move(struct dentry *dentry, struct dentry *new_parent,
		 const char *new_name, size_t new_namelen);
void dget(struct dentry *dentry);
void dput(struct dentry *dentry);
void dcache_init(void);

extern struct dentry *root_dentry;

#define LOOKUP_NOFOLLOW         0x0001
#define LOOKUP_NO_MOUNT         0x0002

#define VFS_MAY_EXEC            0x1
#define VFS_MAY_WRITE           0x2
#define VFS_MAY_READ            0x4

__must_check
int path_lookupat_path(const struct path *base, const char *path,
				    uint32_t flags, struct path *res);

__must_check
int path_parent_lookupat_path(const struct path *base,
					   const char *path, char *name,
					   size_t *namelen, struct path *res);

__must_check
int vfs_at_lookup(int dfd, const char *path, int at_flags, uint32_t lookup_flags,
			       struct vfs_at_lookup_result *res);

void vfs_at_lookup_put(struct vfs_at_lookup_result *res);

__must_check
int vfs_at_base_path(int dfd, const char *path, struct path *res);

__must_check
int vfs_init_inode_owner(struct inode *inode);

__must_check
int vfs_inode_permission(struct inode *inode, uint32_t mask);

__must_check
int vfs_readlink(struct dentry *dentry, char *buf, size_t size);

__must_check
int vfs_create_at_path(const struct path *base, const char *path, uint32_t mode,
		struct path *res);

__must_check
int vfs_symlink_at_path(const struct path *base, const char *target,
		const char *linkpath);

__must_check
int vfs_link_at_path(struct dentry *old_dentry, const struct path *new_base,
				  const char *new_path);

__must_check
int vfs_mkdir_at_path(const struct path *base, const char *path,
				   uint32_t mode);
int vfs_unlink_at_path(const struct path *base, const char *path, int flags);

__must_check
int vfs_rename_at_path(const struct path *old_base, const char *old_path,
		const struct path *new_base, const char *new_path, unsigned int flags);

__must_check
int vfs_mknod_at_path(const struct path *base, const char *path,
				   uint32_t mode, dev_t dev);

__must_check
int vfs_stat_dentry(struct dentry *dentry, struct stat *st);

__must_check
int vfs_chdir_path(const struct path *path);
void mntget(struct vfsmount *mnt);
void mntput(struct vfsmount *mnt);
void path_get(const struct path *path);
void path_put(struct path *path);

__must_check
int vfs_root_path(struct path *path);

__must_check
int vfs_path_from_dentry(struct dentry *dentry, struct path *path);

__must_check
int vfs_mount_root(dev_t dev);

__must_check
int vfs_sync_all(void);

__must_check
int vfs_mount(const char *source, const char *target, const char *type,
		unsigned long flags, const void *data);

__must_check
int vfs_umount(const char *target, int flags);

__must_check
int vfs_follow_mount(struct path *path);

__must_check
int vfs_follow_dotdot_mount(struct path *path);

__must_check
int vfs_register_chrdev(dev_t dev, const struct file_operations *fops);

__must_check
const struct file_operations *vfs_chrdev_fops(dev_t dev);

__must_check
int register_filesystem(struct file_system_type *fs_type);

__must_check
struct file_system_type *get_filesystem_type(const char *name);

__must_check
struct file_system_type *get_next_filesystem_type(struct file_system_type *prev);

__must_check
struct super_block *super_alloc(struct file_system_type *fs_type,
					     dev_t dev);
void vfs_init(void);

__must_check
int filesystems_init(void);

#endif

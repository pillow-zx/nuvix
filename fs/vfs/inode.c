/*
 * fs/vfs/inode.c - Inode 缓存
 */

#include <kernel/errno.h>
#include <kernel/fs.h>
#include <kernel/hash.h>
#include <kernel/slab.h>
#include <kernel/stat.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/vfs.h>

#define ICACHE_HASH_BITS 6
#define ICACHE_HASH_SIZE (1u << ICACHE_HASH_BITS)

HASH_TABLE_DECLARE_STATIC(inode_hashtable, ICACHE_HASH_BITS);

static uint32_t inode_hash(dev_t dev, uint64_t ino)
{
	return (uint32_t)(dev ^ ino ^ (ino >> ICACHE_HASH_BITS)) &
	       (ICACHE_HASH_SIZE - 1);
}

void icache_init(void)
{
	hash_table_init(&inode_hashtable);
}

struct inode *inode_alloc(struct super_block *sb, uint64_t ino)
{
	struct inode *inode = kmalloc(sizeof(*inode), ALLOC_NOWAIT);
	if (!inode)
		return NULL;

	memset(inode, 0, sizeof(*inode));
	inode->i_ino = ino;
	inode->i_sb = sb;
	refcount_set(&inode->i_refcount, 1);
	page_mapping_init(&inode->i_pages, inode, 0, NULL);
	INIT_LIST_HEAD(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_sb_list);

	if (sb)
		list_add_tail(&inode->i_sb_list, &sb->s_inodes);

	return inode;
}

static void inode_hash_insert(struct inode *inode)
{
	uint32_t hash = inode_hash(inode->i_sb->s_dev, inode->i_ino);

	hash_table_add(&inode_hashtable, hash, &inode->i_hash);
}

struct inode *iget(struct super_block *sb, uint64_t ino)
{
	if (!sb)
		return NULL;

	uint32_t hash = inode_hash(sb->s_dev, ino);
	struct list_head *pos;

	hash_table_for_each_possible (pos, &inode_hashtable, hash) {
		struct inode *inode = list_entry(pos, struct inode, i_hash);

		if (inode->i_sb == sb && inode->i_ino == ino) {
			igrab(inode);
			return inode;
		}
	}

	struct inode *inode = inode_alloc(sb, ino);
	if (!inode)
		return NULL;

	if (sb->s_op && sb->s_op->read_inode) {
		int ret = sb->s_op->read_inode(inode);
		if (ret < 0) {
			list_del(&inode->i_sb_list);
			kfree(inode);
			return NULL;
		}
	}

	inode_hash_insert(inode);
	return inode;
}

void igrab(struct inode *inode)
{
	if (inode)
		refcount_inc_allow_zero(&inode->i_refcount);
}

void iput(struct inode *inode)
{
	bool last;

	if (!inode)
		return;

	last = refcount_dec_if_positive(&inode->i_refcount);
	if (last && inode->i_nlink == 0)
		inode_forget(inode);
}

int vfs_inode_writeback(struct inode *inode)
{
	if (!inode || !inode->i_sb || !inode->i_sb->s_op ||
	    !inode->i_sb->s_op->write_inode)
		return -EINVAL;

	return inode->i_sb->s_op->write_inode(inode);
}

int vfs_inode_datasync(struct inode *inode)
{
	if (!inode || !inode->i_sb || !inode->i_sb->s_op)
		return -EINVAL;
	if (inode->i_sb->s_op->datasync_inode)
		return inode->i_sb->s_op->datasync_inode(inode);

	return vfs_inode_writeback(inode);
}

static int64_t vfs_current_time_sec(void)
{
	struct timespec ts;

	kernel_realtime_now(&ts);
	return ts.tv_sec;
}

int vfs_init_inode_owner(struct inode *inode)
{
	int64_t now;

	if (!inode)
		return 0;

	now = vfs_current_time_sec();
	inode->i_uid = task_uid(current_task());
	inode->i_gid = task_gid(current_task());
	inode->i_atime_sec = now;
	inode->i_mtime_sec = now;
	inode->i_ctime_sec = now;
	return vfs_inode_writeback(inode);
}

int vfs_inode_touch(struct inode *inode, bool atime, bool mtime, bool ctime)
{
	int64_t now;

	if (!inode)
		return -EINVAL;

	now = vfs_current_time_sec();
	if (atime)
		inode->i_atime_sec = now;
	if (mtime)
		inode->i_mtime_sec = now;
	if (ctime)
		inode->i_ctime_sec = now;

	return vfs_inode_writeback(inode);
}

int vfs_inode_set_timestamps(struct inode *inode, int64_t atime_sec,
			     int64_t mtime_sec, bool set_atime, bool set_mtime)
{
	if (!inode)
		return -EINVAL;

	if (set_atime)
		inode->i_atime_sec = atime_sec;
	if (set_mtime)
		inode->i_mtime_sec = mtime_sec;
	if (!set_atime && !set_mtime)
		return 0;
	inode->i_ctime_sec = vfs_current_time_sec();

	return vfs_inode_writeback(inode);
}

static int vfs_inode_setattr_permission(const struct inode *inode,
					uint32_t valid)
{
	uid_t uid = task_uid(current_task());

	if ((valid & VFS_ATTR_MODE) && uid != 0 && uid != vfs_inode_uid(inode))
		return -EPERM;
	if ((valid & (VFS_ATTR_UID | VFS_ATTR_GID)) && uid != 0)
		return -EPERM;

	return 0;
}

int vfs_inode_setattr(struct inode *inode, const struct vfs_inode_attrs *attrs)
{
	uint32_t old_mode;
	uint32_t old_uid;
	uint32_t old_gid;
	int64_t old_ctime;
	int ret;

	if (!inode || !attrs)
		return -EINVAL;
	if (attrs->valid & ~VFS_ATTR_ALL)
		return -EINVAL;
	if (!attrs->valid)
		return 0;

	ret = vfs_inode_setattr_permission(inode, attrs->valid);
	if (ret < 0)
		return ret;
	if ((attrs->valid & VFS_ATTR_MODE) && S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;

	old_mode = inode->i_mode;
	old_uid = inode->i_uid;
	old_gid = inode->i_gid;
	old_ctime = inode->i_ctime_sec;
	if (attrs->valid & VFS_ATTR_MODE)
		inode->i_mode = (inode->i_mode & ~VFS_MODE_CHMOD_MASK) |
				(attrs->mode & VFS_MODE_CHMOD_MASK);
	if (attrs->valid & VFS_ATTR_UID)
		inode->i_uid = attrs->uid;
	if (attrs->valid & VFS_ATTR_GID)
		inode->i_gid = attrs->gid;
	if ((attrs->valid & (VFS_ATTR_UID | VFS_ATTR_GID)) &&
	    !S_ISDIR(inode->i_mode))
		inode->i_mode &= ~VFS_MODE_SETID_MASK;
	inode->i_ctime_sec = vfs_current_time_sec();

	ret = vfs_inode_writeback(inode);
	if (ret < 0) {
		inode->i_mode = old_mode;
		inode->i_uid = old_uid;
		inode->i_gid = old_gid;
		inode->i_ctime_sec = old_ctime;
	}

	return ret;
}

int vfs_inode_truncate(struct inode *inode, uint64_t size)
{
	if (!inode || !inode->i_op || !inode->i_op->truncate)
		return -EINVAL;

	return inode->i_op->truncate(inode, size);
}

int vfs_inode_permission(struct inode *inode, uint32_t mask)
{
	uint32_t perm;
	uint32_t want = 0;

	if (!inode)
		return -ENOENT;
	if (!mask)
		return 0;

	if (task_uid(current_task()) == 0)
		return 0;

	if (task_uid(current_task()) == vfs_inode_uid(inode))
		perm = (vfs_inode_mode(inode) >> 6) & 7;
	else if (task_gid(current_task()) == vfs_inode_gid(inode))
		perm = (vfs_inode_mode(inode) >> 3) & 7;
	else
		perm = inode->i_mode & 7;

	if (mask & VFS_MAY_READ)
		want |= 4;
	if (mask & VFS_MAY_WRITE)
		want |= 2;
	if (mask & VFS_MAY_EXEC)
		want |= 1;

	return (perm & want) == want ? 0 : -EACCES;
}

int vfs_stat_inode(const struct inode *inode, struct stat *st)
{
	uint64_t size;

	if (!st)
		return -EINVAL;

	memset(st, 0, sizeof(*st));
	if (!inode)
		return 0;

	size = vfs_inode_size(inode);
	st->st_dev = vfs_inode_dev(inode);
	st->st_ino = vfs_inode_number(inode);
	st->st_mode = vfs_inode_mode(inode);
	st->st_nlink = vfs_inode_nlink(inode);
	st->st_uid = vfs_inode_uid(inode);
	st->st_gid = vfs_inode_gid(inode);
	st->st_rdev = vfs_inode_rdev(inode);
	st->st_size = (int64_t)size;
	if (inode->i_sb && inode->i_sb->s_blocksize)
		st->st_blksize = inode->i_sb->s_blocksize;
	else
		st->st_blksize = 1024;
	st->st_blocks = vfs_inode_blocks(inode);
	st->st_atime_sec = vfs_inode_atime_sec(inode);
	st->st_mtime_sec = vfs_inode_mtime_sec(inode);
	st->st_ctime_sec = vfs_inode_ctime_sec(inode);
	return 0;
}

void inode_forget(struct inode *inode)
{
	if (!inode)
		return;

	if (inode->i_hash.next && inode->i_hash.prev)
		hash_table_del(&inode->i_hash);
	if (inode->i_sb_list.next && inode->i_sb_list.prev)
		list_del(&inode->i_sb_list);

	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->evict_inode)
		inode->i_sb->s_op->evict_inode(inode);

	kfree(inode);
}

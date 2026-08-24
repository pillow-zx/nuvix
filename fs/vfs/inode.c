/*
 * fs/vfs/inode.c - Inode 缓存
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/hashtable.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/stat.h>
#include <nuvix/task.h>
#include <nuvix/time.h>
#include <nuvix/vfs.h>

#define ICACHE_HASH_BITS 6
#define ICACHE_HASH_SIZE (1u << ICACHE_HASH_BITS)

HASH_TABLE_DECLARE_STATIC(inode_hashtable, ICACHE_HASH_BITS);

extern spinlock_t vfs_cache_lock;

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
	mutex_init(&inode->i_lock, LOCK_RANK_INODE, LOCK_IRQ_TASK_ONLY);
	page_mapping_init(&inode->i_pages, inode, 0, NULL);
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_sb_list);

	/* The caller links the inode into sb->s_inodes under vfs_cache_lock. */
	return inode;
}

static void inode_hash_insert(struct inode *inode)
{
	uint32_t hash = inode_hash(inode->i_sb->s_dev, inode->i_ino);

	hash_table_add(&inode_hashtable, hash, &inode->i_hash);
}

struct inode *iget(struct super_block *sb, uint64_t ino)
{
	struct inode *inode;
	uint32_t hash;
	struct hlist_node *pos;

	if (!sb)
		return NULL;

	hash = inode_hash(sb->s_dev, ino);

	spin_lock(&vfs_cache_lock);
	hash_table_for_each_possible (pos, &inode_hashtable, hash) {
		inode = hlist_entry(pos, struct inode, i_hash);

		if (inode->i_sb == sb && inode->i_ino == ino) {
			igrab(inode);
			spin_unlock(&vfs_cache_lock);
			return inode;
		}
	}
	spin_unlock(&vfs_cache_lock);

	inode = inode_alloc(sb, ino);
	if (!inode)
		return NULL;

	if (sb->s_op && sb->s_op->read_inode) {
		int ret = sb->s_op->read_inode(inode);

		if (ret < 0) {
			kfree(inode->i_private);
			kfree(inode);
			return NULL;
		}
	}

	spin_lock(&vfs_cache_lock);
	hash_table_for_each_possible (pos, &inode_hashtable, hash) {
		struct inode *existing = hlist_entry(pos, struct inode, i_hash);

		if (existing->i_sb == sb && existing->i_ino == ino) {
			igrab(existing);
			spin_unlock(&vfs_cache_lock);
			/* read_inode ran outside the cache lock; the loser's
			 * private state is heap-only and never published. */
			kfree(inode->i_private);
			kfree(inode);
			return existing;
		}
	}

	list_add_tail(&inode->i_sb_list, &sb->s_inodes);
	inode_hash_insert(inode);
	spin_unlock(&vfs_cache_lock);
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
	else if (task_in_group(current_task(), vfs_inode_gid(inode)))
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

	spin_lock(&vfs_cache_lock);
	if (!hlist_unhashed(&inode->i_hash))
		hash_table_del(&inode->i_hash);
	if (inode->i_sb_list.next && inode->i_sb_list.prev)
		list_del(&inode->i_sb_list);
	spin_unlock(&vfs_cache_lock);

	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->evict_inode)
		inode->i_sb->s_op->evict_inode(inode);

	kfree(inode);
}

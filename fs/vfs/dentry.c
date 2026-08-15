/*
 * fs/vfs/dentry.c - Dentry 缓存
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/hash.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/vfs.h>

#define DCACHE_HASH_BITS 7
#define DCACHE_HASH_SIZE (1u << DCACHE_HASH_BITS)

HASH_TABLE_DECLARE_STATIC(dentry_hashtable, DCACHE_HASH_BITS);

/*
 * One global lock serializes the dentry and inode hash tables, each
 * superblock's inode list, and each dentry's child list.  All VFS cache
 * mutations run under it; frees and eviction run after it is released.
 */
DEFINE_SPINLOCK(vfs_cache_lock, LOCK_RANK_VFS_CACHE);

static uint32_t dentry_hash(struct dentry *parent, const char *name,
			    size_t namelen)
{
	uintptr_t key = (uintptr_t)parent;

	for (size_t i = 0; i < namelen; i++)
		key = (key * 33u) ^ (uint8_t)name[i];

	return (uint32_t)key & (DCACHE_HASH_SIZE - 1);
}

static bool dentry_hashed(struct dentry *dentry)
{
	return dentry && dentry->d_hash.next && dentry->d_hash.prev &&
	       !list_empty(&dentry->d_hash);
}

static void dcache_insert_locked(struct dentry *dentry)
{
	uint32_t hash;

	if (!dentry || !dentry->d_parent || dentry_hashed(dentry))
		return;

	hash = dentry_hash(dentry->d_parent, dentry->d_name,
			   dentry->d_namelen);
	hash_table_add(&dentry_hashtable, hash, &dentry->d_hash);
}

void dcache_init(void)
{
	hash_table_init(&dentry_hashtable);
}

struct dentry *dentry_alloc(struct dentry *parent, const char *name,
			    size_t namelen)
{
	if (!name || namelen > VFS_NAME_MAX)
		return NULL;

	struct dentry *dentry = kmalloc(sizeof(*dentry), ALLOC_NOWAIT);
	if (!dentry)
		return NULL;

	memset(dentry, 0, sizeof(*dentry));
	memcpy(dentry->d_name, name, namelen);
	dentry->d_name[namelen] = '\0';
	dentry->d_namelen = (uint8_t)namelen;
	refcount_set(&dentry->d_refcount, 1);
	dentry->d_parent = parent ? parent : dentry;
	dentry->d_sb = parent ? parent->d_sb : NULL;
	INIT_LIST_HEAD(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_child);
	INIT_LIST_HEAD(&dentry->d_subdirs);

	if (parent) {
		spin_lock(&vfs_cache_lock);
		list_add_tail(&dentry->d_child, &parent->d_subdirs);
		spin_unlock(&vfs_cache_lock);
	}

	return dentry;
}

struct dentry *dcache_lookup(struct dentry *parent, const char *name,
			     size_t namelen)
{
	if (!parent || !name || namelen > VFS_NAME_MAX)
		return NULL;

	uint32_t hash = dentry_hash(parent, name, namelen);
	struct list_head *pos;

	spin_lock(&vfs_cache_lock);
	hash_table_for_each_possible (pos, &dentry_hashtable, hash) {
		struct dentry *dentry = list_entry(pos, struct dentry, d_hash);

		if (dentry->d_parent != parent || dentry->d_namelen != namelen)
			continue;
		if (memcmp(dentry->d_name, name, namelen) != 0)
			continue;

		dget(dentry);
		spin_unlock(&vfs_cache_lock);
		return dentry;
	}
	spin_unlock(&vfs_cache_lock);

	return NULL;
}

void dcache_insert(struct dentry *dentry)
{
	spin_lock(&vfs_cache_lock);
	dcache_insert_locked(dentry);
	spin_unlock(&vfs_cache_lock);
}

void dcache_invalidate(struct dentry *dentry)
{
	if (!dentry)
		return;

	spin_lock(&vfs_cache_lock);
	if (dentry_hashed(dentry))
		hash_table_del(&dentry->d_hash);
	else
		INIT_LIST_HEAD(&dentry->d_hash);

	dentry->d_inode = NULL;
	spin_unlock(&vfs_cache_lock);
}

void dcache_move(struct dentry *dentry, struct dentry *new_parent,
		 const char *new_name, size_t new_namelen)
{
	if (!dentry || !new_parent || !new_name || new_namelen > VFS_NAME_MAX)
		return;

	spin_lock(&vfs_cache_lock);
	if (dentry_hashed(dentry))
		hash_table_del(&dentry->d_hash);
	else
		INIT_LIST_HEAD(&dentry->d_hash);

	if (dentry->d_parent != new_parent)
		list_move_tail(&dentry->d_child, &new_parent->d_subdirs);

	memcpy(dentry->d_name, new_name, new_namelen);
	dentry->d_name[new_namelen] = '\0';
	dentry->d_namelen = (uint8_t)new_namelen;
	dentry->d_parent = new_parent;
	dentry->d_sb = new_parent->d_sb;
	dcache_insert_locked(dentry);
	spin_unlock(&vfs_cache_lock);
}

void dget(struct dentry *dentry)
{
	if (dentry)
		refcount_inc_allow_zero(&dentry->d_refcount);
}

void dput(struct dentry *dentry)
{
	if (!dentry)
		return;

	(void)refcount_dec_if_positive(&dentry->d_refcount);
}

int vfs_stat_dentry(struct dentry *dentry, struct stat *st)
{
	if (!dentry || !st)
		return -EINVAL;

	return vfs_stat_inode(vfs_dentry_inode(dentry), st);
}

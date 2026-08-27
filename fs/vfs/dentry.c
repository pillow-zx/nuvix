/* Dentry cache with bounded residency and two-phase retirement. */

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/hashtable.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/vfs.h>

#define DCACHE_HASH_BITS  7
#define DCACHE_HASH_SIZE  (1u << DCACHE_HASH_BITS)
#define DCACHE_NR_ENTRIES 256U

HASH_TABLE_DECLARE_STATIC(dentry_hashtable, DCACHE_HASH_BITS);
LIST_HEAD_STATIC(dentry_lru);

DEFINE_SPINLOCK(vfs_cache_lock, LOCK_RANK_VFS_CACHE, LOCK_IRQ_TASK_ONLY);
static uint32_t dentry_entries;

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
	return dentry && !hlist_unhashed(&dentry->d_hash);
}

static void dcache_unpublish_locked(struct dentry *dentry)
{
	if (!dentry || !dentry->d_resident)
		return;
	if (dentry_hashed(dentry))
		hash_table_del(&dentry->d_hash);
	if (!list_empty(&dentry->d_lru))
		list_del_init(&dentry->d_lru);
	if (dentry->d_parent != dentry && !list_empty(&dentry->d_child))
		list_del_init(&dentry->d_child);
	dentry->d_resident = false;
	dentry->d_retiring = true;
	BUG_ON(dentry_entries == 0);
	dentry_entries--;
}

static struct dentry *dcache_reclaim_leaf_locked(void)
{
	struct dentry *dentry;

	list_for_each_entry (dentry, &dentry_lru, d_lru) {
		if (refcount_read(&dentry->d_refcount) != 0 ||
		    !list_empty(&dentry->d_subdirs) ||
		    dentry->d_parent == dentry)
			continue;
		dcache_unpublish_locked(dentry);
		return dentry;
	}
	return NULL;
}

static void dentry_free(struct dentry *dentry)
{
	struct inode *inode;

	if (!dentry)
		return;
	inode = dentry->d_inode;
	dentry->d_inode = NULL;
	if (inode)
		iput(inode);
	kfree(dentry);
}

void dcache_init(void)
{
	hash_table_init(&dentry_hashtable);
	INIT_LIST_HEAD(&dentry_lru);
	dentry_entries = 0;
}

struct dentry *dentry_alloc(struct dentry *parent, const char *name,
			    size_t namelen)
{
	struct dentry *dentry;

	if (!name || namelen > VFS_NAME_MAX)
		return NULL;
	dentry = kmalloc(sizeof(*dentry), ALLOC_NOWAIT);
	if (!dentry)
		return NULL;

	memset(dentry, 0, sizeof(*dentry));
	memcpy(dentry->d_name, name, namelen);
	dentry->d_name[namelen] = '\0';
	dentry->d_namelen = (uint8_t)namelen;
	refcount_set(&dentry->d_refcount, 1);
	dentry->d_parent = parent ? parent : dentry;
	dentry->d_sb = parent ? parent->d_sb : NULL;
	INIT_HLIST_NODE(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_child);
	INIT_LIST_HEAD(&dentry->d_subdirs);
	INIT_LIST_HEAD(&dentry->d_lru);
	return dentry;
}

struct dentry *dcache_lookup(struct dentry *parent, const char *name,
			     size_t namelen)
{
	struct hlist_node *pos;
	uint32_t hash;

	if (!parent || !name || namelen > VFS_NAME_MAX)
		return NULL;
	hash = dentry_hash(parent, name, namelen);

	spin_lock(&vfs_cache_lock);
	hash_table_for_each_possible (pos, &dentry_hashtable, hash) {
		struct dentry *dentry = hlist_entry(pos, struct dentry, d_hash);

		if (dentry->d_retiring || dentry->d_parent != parent ||
		    dentry->d_namelen != namelen ||
		    memcmp(dentry->d_name, name, namelen) != 0)
			continue;
		refcount_inc_allow_zero(&dentry->d_refcount);
		list_move_tail(&dentry->d_lru, &dentry_lru);
		spin_unlock(&vfs_cache_lock);
		return dentry;
	}
	spin_unlock(&vfs_cache_lock);
	return NULL;
}

int dcache_insert(struct dentry *dentry)
{
	struct dentry *victim = NULL;
	uint32_t hash;

	if (!dentry || !dentry->d_parent)
		return -EINVAL;
	spin_lock(&vfs_cache_lock);
	if (dentry->d_resident) {
		spin_unlock(&vfs_cache_lock);
		return 0;
	}
	if (dentry_entries >= DCACHE_NR_ENTRIES)
		victim = dcache_reclaim_leaf_locked();
	if (dentry_entries >= DCACHE_NR_ENTRIES) {
		spin_unlock(&vfs_cache_lock);
		return -ENOMEM;
	}
	hash = dentry_hash(dentry->d_parent, dentry->d_name, dentry->d_namelen);
	hash_table_add(&dentry_hashtable, hash, &dentry->d_hash);
	if (dentry->d_parent != dentry)
		list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
	list_add_tail(&dentry->d_lru, &dentry_lru);
	dentry->d_resident = true;
	dentry->d_retiring = false;
	dentry_entries++;
	spin_unlock(&vfs_cache_lock);
	dentry_free(victim);
	return 0;
}

void dcache_invalidate(struct dentry *dentry)
{
	struct inode *inode;
	bool free_dentry;

	if (!dentry)
		return;
	spin_lock(&vfs_cache_lock);
	if (dentry->d_resident && !list_empty(&dentry->d_subdirs)) {
		spin_unlock(&vfs_cache_lock);
		return;
	}
	dcache_unpublish_locked(dentry);
	inode = dentry->d_inode;
	dentry->d_inode = NULL;
	free_dentry = refcount_read(&dentry->d_refcount) == 0;
	spin_unlock(&vfs_cache_lock);
	if (inode)
		iput(inode);
	if (free_dentry)
		kfree(dentry);
}

void dcache_move(struct dentry *dentry, struct dentry *new_parent,
		 const char *new_name, size_t new_namelen)
{
	uint32_t hash;

	if (!dentry || !new_parent || !new_name || new_namelen > VFS_NAME_MAX)
		return;
	spin_lock(&vfs_cache_lock);
	if (dentry_hashed(dentry))
		hash_table_del(&dentry->d_hash);
	if (dentry->d_parent != new_parent) {
		if (!list_empty(&dentry->d_child))
			list_del_init(&dentry->d_child);
		list_add_tail(&dentry->d_child, &new_parent->d_subdirs);
	}
	memcpy(dentry->d_name, new_name, new_namelen);
	dentry->d_name[new_namelen] = '\0';
	dentry->d_namelen = (uint8_t)new_namelen;
	dentry->d_parent = new_parent;
	dentry->d_sb = new_parent->d_sb;
	hash = dentry_hash(new_parent, new_name, new_namelen);
	hash_table_add(&dentry_hashtable, hash, &dentry->d_hash);
	spin_unlock(&vfs_cache_lock);
}

void dget(struct dentry *dentry)
{
	if (dentry)
		refcount_inc_not_zero(&dentry->d_refcount);
}

void dput(struct dentry *dentry)
{
	bool free_dentry = false;

	if (!dentry)
		return;
	spin_lock(&vfs_cache_lock);
	if (refcount_dec_if_positive(&dentry->d_refcount) &&
	    !dentry->d_resident) {
		BUG_ON(!list_empty(&dentry->d_subdirs));
		free_dentry = true;
	}
	spin_unlock(&vfs_cache_lock);
	if (free_dentry)
		dentry_free(dentry);
}

/* Retire @root and its whole cached subtree child-first.  The caller must
 * already hold the only external references (umount detaches the mount and
 * drops its path refs first).  Any pinned entry fails the whole transaction
 * with -EBUSY and leaves everything published; on success every dentry is
 * unpublished and its construction reference to ->d_inode dropped. */
int dcache_retire_subtree(struct dentry *root)
{
	LIST_HEAD(freed);
	struct list_head *pos, *next;
	int ret = 0;

	if (!root)
		return -EINVAL;

	spin_lock(&vfs_cache_lock);

	for (;;) {
		bool progressed = false;
		bool pinned = false;

		list_for_each_safe (pos, next, &root->d_subdirs) {
			struct dentry *child =
				list_entry(pos, struct dentry, d_child);

			if (!list_empty(&child->d_subdirs))
				continue;
			if (refcount_read(&child->d_refcount) != 0) {
				pinned = true;
				break;
			}
			dcache_unpublish_locked(child);
			/* d_lru was detached by unpublish, so it doubles as
			 * the staging node; never alias the object body. */
			list_add_tail(&child->d_lru, &freed);
			progressed = true;
		}

		if (pinned) {
			ret = -EBUSY;
			break;
		}
		if (!progressed)
			break;
	}

	if (ret == 0) {
		if (refcount_read(&root->d_refcount) > 1) {
			ret = -EBUSY;
		} else {
			dcache_unpublish_locked(root);
			list_add_tail(&root->d_lru, &freed);
		}
	}

	spin_unlock(&vfs_cache_lock);

	if (ret < 0)
		return ret;

	list_for_each_safe (pos, next, &freed) {
		struct dentry *dentry = list_entry(pos, struct dentry, d_lru);

		list_del_init(pos);
		dentry_free(dentry);
	}
	return 0;
}

int vfs_stat_dentry(struct dentry *dentry, struct stat *st)
{
	if (!dentry || !st)
		return -EINVAL;

	return vfs_stat_inode(vfs_dentry_inode(dentry), st);
}

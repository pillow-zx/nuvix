#include <nuvix/fs.h>
#include <nuvix/slab.h>
#include <nuvix/assert.h>
#include <nuvix/refcount.h>
#include <nuvix/hashtable.h>
#include <nuvix/spinlock.h>

static DEFINE_SPINLOCK(icache_lock);
static DEFINE_HASHTABLE(icache, 8);

struct inode *i_alloc(void)
{
    struct inode *inode = kzalloc(sizeof(*inode));

    if (!inode)
        return NULL;
    refcount_set(&inode->i_count, 1);
    mutex_init(&inode->mutex);
    return inode;
}

void ihold(struct inode *inode)
{
    refcount_inc(&inode->i_count);
}

void iput(struct inode *inode)
{
    if (!inode)
        return;
    spin_lock(&icache_lock);
    if (!refcount_dec_and_test(&inode->i_count)) {
        spin_unlock(&icache_lock);
        return;
    }
    hash_del(&inode->i_hash);
    spin_unlock(&icache_lock);
    if (inode->i_ops && inode->i_ops->destroy)
        inode->i_ops->destroy(inode);
    else
        kfree(inode);
}

static uint64_t inode_key(struct super_block *sb, uint64_t ino)
{
    return (uint64_t)(uintptr_t)sb ^ ino;
}

static struct inode *icache_find_locked(struct super_block *sb, uint64_t ino)
{
    struct inode *inode;

    assert_spin_locked(&icache_lock);
    hash_for_each_possible (icache, inode, i_hash, inode_key(sb, ino)) {
        if (inode->i_sb == sb && inode->i_ino == ino)
            return inode;
    }
    return NULL;
}

struct inode *icache_find(struct super_block *sb, uint64_t ino)
{
    struct inode *inode;

    spin_lock(&icache_lock);
    inode = icache_find_locked(sb, ino);
    if (inode)
        refcount_inc(&inode->i_count);
    spin_unlock(&icache_lock);
    return inode;
}

struct inode *icache_insert(struct inode *inode)
{
    struct inode *node;

    Assert(!hash_hashed(&inode->i_hash), "inode %lu already published\n",
           (unsigned long)inode->i_ino);

    spin_lock(&icache_lock);
    node = icache_find_locked(inode->i_sb, inode->i_ino);
    if (node) {
        refcount_inc(&node->i_count);
        spin_unlock(&icache_lock);
        iput(inode);
        return node;
    }

    hash_add(icache, &inode->i_hash, inode_key(inode->i_sb, inode->i_ino));
    spin_unlock(&icache_lock);
    return inode;
}

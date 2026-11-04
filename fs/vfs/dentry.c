#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/macro.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/spinlock.h>
#include <nuvix/hashtable.h>

DEFINE_SPINLOCK(dcache_lock);
DEFINE_HASHTABLE(dcache, 8);

struct dentry *d_alloc(struct dentry *parent, const char *name, size_t len)
{
    struct dentry *dentry __free(kfree) = kzalloc(sizeof(*dentry));
    char *d_name __free(kfree) = NULL;

    if (!dentry)
        return ERR_PTR(-ENOMEM);
    d_name = kmalloc(len + 1);
    if (!d_name)
        return ERR_PTR(-ENOMEM);

    memcpy(d_name, name, len);
    d_name[len] = '\0';
    dentry->d_name = move(d_name);
    dentry->d_nlen = (unsigned int)len;
    if (parent) {
        dget(parent);
        dentry->d_parent = parent;
    }
    refcount_set(&dentry->d_count, 1);
    return move(dentry);
}

void dget(struct dentry *dentry)
{
    refcount_inc(&dentry->d_count);
}

void dput(struct dentry *dentry)
{
    while (dentry) {
        struct dentry *parent;
        struct inode *inode;

        spin_lock(&dcache_lock);
        if (!refcount_dec_and_test(&dentry->d_count)) {
            spin_unlock(&dcache_lock);
            break;
        }
        hash_del(&dentry->d_hash);
        spin_unlock(&dcache_lock);
        parent = dentry->d_parent;
        inode = dentry->d_inode;
        if (inode)
            iput(inode);
        kfree(dentry->d_name);
        kfree(dentry);
        dentry = parent;
    }
}

static uint64_t dentry_key(struct dentry *parent, const char *name,
                           unsigned int len)
{
    uint64_t key = (uint64_t)(uintptr_t)parent;

    for (unsigned int i = 0; i < len; ++i)
        key = key * 31 + (unsigned char)name[i];

    return key;
}

static struct dentry *dcache_find_locked(struct dentry *parent,
                                         const char *name, unsigned int len)
{
    struct dentry *dentry;

    assert_spin_locked(&dcache_lock);
    hash_for_each_possible (dcache, dentry, d_hash,
                            dentry_key(parent, name, len)) {
        if (dentry->d_parent == parent && dentry->d_nlen == len &&
            !memcmp(dentry->d_name, name, len))
            return dentry;
    }
    return NULL;
}

struct dentry *dcache_find(struct dentry *parent, const char *name,
                           unsigned int len)
{
    struct dentry *dentry;

    spin_lock(&dcache_lock);
    dentry = dcache_find_locked(parent, name, len);
    if (dentry)
        dget(dentry);
    spin_unlock(&dcache_lock);

    return dentry;
}

struct dentry *dcache_insert(struct dentry *dentry)
{
    struct dentry *entry;

    Assert(!hash_hashed(&dentry->d_hash), "dentry %s already published\n",
           dentry->d_name);

    spin_lock(&dcache_lock);
    entry =
        dcache_find_locked(dentry->d_parent, dentry->d_name, dentry->d_nlen);
    if (entry) {
        dget(entry);
        spin_unlock(&dcache_lock);
        dput(dentry);
        return entry;
    }

    hash_add(dcache, &dentry->d_hash,
             dentry_key(dentry->d_parent, dentry->d_name, dentry->d_nlen));
    spin_unlock(&dcache_lock);
    return dentry;
}

void dcache_drop(struct dentry *dentry)
{
    spin_lock(&dcache_lock);
    hash_del(&dentry->d_hash);
    spin_unlock(&dcache_lock);
}

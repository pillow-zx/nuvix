#include <nuvix/vfs.h>

#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>

static struct dcache dcache = {.lock = SPINLOCK_INITIALIZER};

static uint64_t name_hash(struct dentry *parent, const char *name, size_t len)
{
    uint64_t hash = (uintptr_t)parent;

    for (size_t i = 0; i < len; ++i)
        hash = hash * 31 + (uint8_t)name[i];
    return hash;
}

struct dentry *dentry_alloc(struct super_block *sb, struct dentry *parent,
                            const char *name, size_t len)
{
    struct dentry *dentry __free(kfree) = NULL;

    dentry = kzalloc(sizeof(*dentry));
    if (!dentry)
        return NULL;
    dentry->d_name = kmalloc(len + 1);
    if (!dentry->d_name)
        return NULL;
    memcpy(dentry->d_name, name, len);
    dentry->d_name[len] = '\0';
    dentry->d_name_len = len;
    dentry->d_sb = sb;
    dentry->d_parent = parent ? dget(parent) : dentry;
    dentry->d_name_hash = name_hash(dentry->d_parent, name, len);
    INIT_HLIST_NODE(&dentry->d_hash);
    refcount_set(&dentry->d_count, 1);
    return move(dentry);
}

struct dentry *dget(struct dentry *dentry)
{
    unsigned long flags;

    if (!dentry)
        return NULL;
    spin_lock_irqsave(&dcache.lock, flags);
    refcount_inc(&dentry->d_count);
    spin_unlock_irqrestore(&dcache.lock, flags);
    return dentry;
}

void dput(struct dentry *dentry)
{
    unsigned long flags;

    if (!dentry)
        return;
    might_sleep();
    while (dentry) {
        struct dentry *parent;
        struct inode *inode;

        spin_lock_irqsave(&dcache.lock, flags);
        if (!refcount_dec_and_test(&dentry->d_count)) {
            spin_unlock_irqrestore(&dcache.lock, flags);
            return;
        }
        hash_del(&dentry->d_hash);
        parent = dentry->d_parent == dentry ? NULL : dentry->d_parent;
        inode = dentry->d_inode;
        spin_unlock_irqrestore(&dcache.lock, flags);

        if (inode)
            iput(inode);
        kfree(dentry->d_name);
        kfree(dentry);
        dentry = parent;
    }
}

void ddrop(struct dentry *dentry)
{
    unsigned long flags;

    Assert(dentry->d_parent != dentry, "cannot drop a root dentry\n");
    spin_lock_irqsave(&dcache.lock, flags);
    hash_del(&dentry->d_hash);
    spin_unlock_irqrestore(&dcache.lock, flags);
}

static struct dentry *dcache_lookup(struct dentry *parent, const char *name,
                                    size_t len, uint64_t hash)
{
    struct dentry *dentry;
    unsigned long flags;

    spin_lock_irqsave(&dcache.lock, flags);
    hash_for_each_possible (dcache.names, dentry, d_hash, hash) {
        if (dentry->d_parent == parent && dentry->d_name_hash == hash &&
            dentry->d_name_len == len && !memcmp(dentry->d_name, name, len)) {
            refcount_inc(&dentry->d_count);
            spin_unlock_irqrestore(&dcache.lock, flags);
            return dentry;
        }
    }
    spin_unlock_irqrestore(&dcache.lock, flags);
    return NULL;
}

struct dentry *dentry_lookup(struct dentry *parent, const char *name, size_t len)
{
    struct inode *dir = parent->d_inode;
    struct dentry *dentry __free(dput) = NULL;
    struct inode *inode __free(iput) = NULL;
    uint64_t hash, ino;
    unsigned long flags;
    int error;

    might_sleep();
    if (!dir)
        return ERR_PTR(-ENOENT);
    if (!S_ISDIR(dir->i_mode))
        return ERR_PTR(-ENOTDIR);
    if (!len)
        return ERR_PTR(-EINVAL);
    if (len > VFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);
    if (memchr(name, '\0', len) || memchr(name, '/', len))
        return ERR_PTR(-EINVAL);
    if (len == 1 && name[0] == '.')
        return dget(parent);
    if (len == 2 && name[0] == '.' && name[1] == '.')
        return dget(parent->d_parent);

    hash = name_hash(parent, name, len);
    dentry = dcache_lookup(parent, name, len, hash);
    if (dentry)
        return move(dentry);
    if (!dir->i_ops || !dir->i_ops->lookup)
        return ERR_PTR(-EOPNOTSUPP);

    dentry = dentry_alloc(parent->d_sb, parent, name, len);
    if (!dentry)
        return ERR_PTR(-ENOMEM);
    error = dir->i_ops->lookup(dir, name, len, &ino);
    if (error && error != -ENOENT)
        return ERR_PTR(error);
    if (!error) {
        inode = iget(parent->d_sb, ino);
        if (IS_ERR(inode))
            return ERR_PTR(PTR_ERR(inode));
        dentry->d_inode = move(inode);
    }

    /* Namespace excludes other publishers; dput may still remove old entries. */
    spin_lock_irqsave(&dcache.lock, flags);
    hash_add(dcache.names, &dentry->d_hash, dentry->d_name_hash);
    spin_unlock_irqrestore(&dcache.lock, flags);
    return move(dentry);
}

void path_get(const struct path *path)
{
    dget(path->dentry);
}

void path_put(const struct path *path)
{
    dput(path->dentry);
}

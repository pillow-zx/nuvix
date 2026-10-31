#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/macro.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>

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

        if (!refcount_dec_and_test(&dentry->d_count))
            break;
        parent = dentry->d_parent;
        inode = dentry->d_inode;
        if (inode)
            iput(inode);
        kfree(dentry->d_name);
        kfree(dentry);
        dentry = parent;
    }
}

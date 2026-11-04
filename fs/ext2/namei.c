#include <nuvix/errno.h>

#include "ext2.h"

static struct dentry *ext2_lookup_locked(struct inode *dir,
                                         struct dentry *dentry)
{
    struct inode *inode;
    uint32_t ino;
    int ret;

    if (!S_ISDIR(dir->i_mode))
        return ERR_PTR(-ENOTDIR);
    if (dentry->d_nlen > VFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);

    ret = ext2_inode_by_name(dir, dentry->d_name, dentry->d_nlen, &ino);
    if (ret == -ENOENT)
        return dentry;
    if (ret)
        return ERR_PTR(ret);

    inode = ext2_iget(dir->i_sb, ino);
    if (IS_ERR(inode))
        return ERR_PTR(PTR_ERR(inode));

    dentry->d_inode = inode;
    return dentry;
}

struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
    struct dentry *found;

    mutex_lock(&dir->mutex);
    found = ext2_lookup_locked(dir, dentry);
    mutex_unlock(&dir->mutex);
    return found;
}

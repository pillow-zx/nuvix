#include <nuvix/errno.h>

#include "ext2.h"

struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode;
    uint32_t ino;
    int ret;

    assert_mutex_locked(&dir->mutex);
    if (!S_ISDIR(dir->i_mode))
        return ERR_PTR(-ENOTDIR);
    if (dentry->d_nlen > VFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);

    ret = ext2_find_ino(dir, dentry, &ino);
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

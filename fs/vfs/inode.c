#include <nuvix/fs.h>
#include <nuvix/slab.h>

struct inode *i_alloc(void)
{
    struct inode *inode = kzalloc(sizeof(*inode));

    if (!inode)
        return NULL;
    refcount_set(&inode->i_count, 1);
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
    if (!refcount_dec_and_test(&inode->i_count))
        return;
    if (inode->i_ops && inode->i_ops->destroy)
        inode->i_ops->destroy(inode);
    kfree(inode);
}

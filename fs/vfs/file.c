#include <nuvix/vfs.h>

#include <nuvix/errno.h>
#include <nuvix/slab.h>

struct file *file_open(const struct path *path, unsigned int mode,
                       unsigned int flags)
{
    struct inode *inode = path->dentry->d_inode;
    struct file *file __free(kfree) = NULL;
    struct path file_path __free(path_put) = {0};
    int error;

    might_sleep();
    if (!mode || (mode & ~(FMODE_READ | FMODE_WRITE)))
        return ERR_PTR(-EINVAL);
    if (!inode)
        return ERR_PTR(-ENOENT);
    if (!inode->i_fops)
        return ERR_PTR(-EOPNOTSUPP);

    file = kzalloc(sizeof(*file));
    if (!file)
        return ERR_PTR(-ENOMEM);
    refcount_set(&file->f_count, 1);
    file_path = *path;
    path_get(&file_path);
    file->f_path = file_path;
    file->f_inode = inode;
    file->f_ops = inode->i_fops;
    file->f_mode = mode;
    file->f_flags = flags;
    mutex_init(&file->f_pos_lock);

    if (file->f_ops->open) {
        error = file->f_ops->open(inode, file);
        if (error)
            return ERR_PTR(error);
    }
    keep(file_path);
    return move(file);
}

struct file *fget(struct file *file)
{
    if (file)
        refcount_inc(&file->f_count);
    return file;
}

void fput(struct file *file)
{
    if (!file)
        return;
    might_sleep();
    if (!refcount_dec_and_test(&file->f_count))
        return;
    if (file->f_ops->release)
        file->f_ops->release(file);
    path_put(&file->f_path);
    kfree(file);
}

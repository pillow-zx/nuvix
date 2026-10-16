#include <nuvix/vfs.h>
#include <nuvix/errno.h>
#include <nuvix/refcount.h>
#include <nuvix/slab.h>
#include <nuvix/fat.h>
#include <nuvix/mutex.h>
#include <nuvix/macro.h>

struct file *vfs_open(const char *path, unsigned long flags)
{
    struct file *file __free(kfree) = NULL;
    int ret;

    file = kzalloc(sizeof(*file));
    if (!file)
        return ERR_PTR(-ENOMEM);

    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_pos = 0;

    ret = fat_open(file, path, flags);
    if (ret)
        return ERR_PTR(ret);

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
    if (!refcount_dec_and_test(&file->f_count))
        return;
    if (file->f_ops && file->f_ops->release)
        file->f_ops->release(file);
    kfree(file);
}

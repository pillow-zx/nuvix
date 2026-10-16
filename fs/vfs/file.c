
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

#include <nuvix/errno.h>
#include <nuvix/vfs.h>
#include "proc.h"

static int filesystems_show(struct inode *inode, char *buf)
{
    const struct file_system_type *type = NULL;
    int len = 0;

    (void)inode;
    while ((type = vfs_next_type(type))) {
        int n = proc_format(buf ? buf + len : NULL, "%s\t%s\n",
                            type->nodev ? "nodev" : "", type->name);

        if (n < 0)
            return n;
        if (check_add_overflow(len, n, &len))
            return -EOVERFLOW;
    }
    return len;
}

static int filesystems_open(struct file *file)
{
    int len = filesystems_show(file->f_inode, NULL);

    if (len < 0)
        return len;
    return proc_open(file, (size_t)len + 1, filesystems_show);
}

const struct file_operations filesystems_fops = {
    .open = filesystems_open,
    .llseek = proc_llseek,
    .read = proc_read,
    .release = proc_release,
};

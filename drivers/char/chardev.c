#include <nuvix/chardev.h>
#include <nuvix/fs.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>

static LIST_HEAD(chardevs);

static struct chardev *chardev_find(dev_t dev)
{
    struct chardev *entry;

    list_for_each_entry (entry, &chardevs, node)
        if (entry->dev == dev)
            return entry;
    return NULL;
}

void chardev_register(struct chardev *dev)
{
    list_add_tail(&dev->node, &chardevs);
}

static int chardev_file_open(struct file *file)
{
    return chardev_open(file->f_inode->i_rdev, file);
}

static const struct file_operations chardev_fops = {
    .open = chardev_file_open,
};

void chardev_init(struct inode *inode, dev_t dev)
{
    inode->i_rdev = dev;
    inode->i_fops = &chardev_fops;
}

int chardev_open(dev_t dev, struct file *file)
{
    struct chardev *entry = chardev_find(dev);

    if (!entry)
        return -ENXIO;
    file->f_ops = entry->fops;
    file->f_private = entry->private;
    return file->f_ops->open ? file->f_ops->open(file) : 0;
}

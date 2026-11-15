#ifndef _NUVIX_CHARDEV_H
#define _NUVIX_CHARDEV_H

#include <nuvix/device.h>

struct file;
struct file_operations;
struct inode;

struct chardev {
    struct list_head node;
    dev_t dev;
    const struct file_operations *fops;
    void *private;
};

#define CHARDEV_INIT(device, ops, priv)                                        \
    (struct chardev)                                                           \
    {                                                                          \
        .dev = device, .fops = ops, .private = priv                            \
    }

void chardev_register(struct chardev *dev);

void chardev_init(struct inode *inode, dev_t dev);

int chardev_open(dev_t dev, struct file *file);

#endif /* _NUVIX_CHARDEV_H */

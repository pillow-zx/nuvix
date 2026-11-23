#include <nuvix/errno.h>
#include <nuvix/kiov.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include "proc.h"

struct proc_text {
    struct mutex lock;
    size_t len;
    int (*show)(struct inode *, char *);
    char data[];
};

int proc_open(struct file *file, size_t size,
                   int (*show)(struct inode *, char *))
{
    struct proc_text *text = kzalloc(sizeof(*text) + size);

    if (!text)
        return -ENOMEM;
    mutex_init(&text->lock);
    text->show = show;
    file->f_private = text;
    return 0;
}

ssize_t proc_read(struct kiocb *iocb, struct kiov *iov)
{
    struct proc_text *text = iocb->file->f_private;
    ssize_t ret = 0;

    mutex_lock(&text->lock);
    if (!iocb->pos || !text->len) {
        int len = text->show(iocb->file->f_inode, text->data);

        if (len < 0) {
            ret = len;
            goto out;
        }
        text->len = len;
    }
    if ((uint64_t)iocb->pos < text->len) {
        size_t pos = iocb->pos;
        size_t copied = copy_to_kiov(text->data + pos, text->len - pos, iov);

        iocb->pos += copied;
        ret = copied ? (ssize_t)copied : -EFAULT;
    }
out:
    mutex_unlock(&text->lock);
    return ret;
}

int proc_release(struct file *file)
{
    kfree(file->f_private);
    return 0;
}

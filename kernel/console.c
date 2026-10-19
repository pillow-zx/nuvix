#include <nuvix/console.h>
#include <nuvix/vfs.h>
#include <nuvix/fdtable.h>
#include <nuvix/ns16550a.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>

static ssize_t console_read(struct file *file, char *buffer, size_t count,
                            loff_t *pos)
{
    (void)file;
    (void)pos;
    return ns16550a_read(buffer, count);
}

static ssize_t console_write(struct file *file, const char *buffer,
                             size_t count, loff_t *pos)
{
    (void)file;
    (void)pos;
    return ns16550a_write(buffer, count);
}

static const struct file_operations console_fops = {
    .read = console_read,
    .write = console_write,
};

static struct file *console_open(unsigned int flags)
{
    struct file *file = kzalloc(sizeof(*file));

    if (!file)
        return NULL;
    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_ops = &console_fops;
    return file;
}

int console_init(struct fdtable *fdt)
{
    struct file *input __free(fput) = NULL;
    struct file *output __free(fput) = NULL;
    struct file *error __free(fput) = NULL;

    input = console_open(O_RDONLY);
    if (!input)
        return -ENOMEM;
    output = console_open(O_WRONLY);
    if (!output)
        return -ENOMEM;
    error = console_open(O_WRONLY);

    fd_install(fdt, stdin, move(input));
    fd_install(fdt, stdout, move(output));
    fd_install(fdt, stderr, move(error));
    return 0;
}

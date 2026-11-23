#include <nuvix/page.h>
#include <nuvix/slab.h>
#include "proc.h"

#define MEMINFO_FORMAT                                                         \
    "MemTotal: %llu kB\nMemFree:  %llu kB\nSlab:     %llu kB\n"

static int meminfo_show(struct inode *inode, char *buf)
{
    unsigned long total, free;

    (void)inode;
    buddy_meminfo(&total, &free);

#define INFO(x) ((unsigned long long)x * (PAGE_SIZE / 1024))
    return proc_format(buf, MEMINFO_FORMAT,
            INFO(total), INFO(free), INFO(slab_pages()));
#undef INFO
}

static int meminfo_open(struct file *file)
{
    return proc_open(file,
            sizeof(MEMINFO_FORMAT) + 3 * PROC_U64_DIGITS,
            meminfo_show);
}

const struct file_operations meminfo_fops = {
    .open = meminfo_open,
    .llseek = proc_llseek,
    .read = proc_read,
    .release = proc_release,
};


#define BUDDYINFO_PREFIX "Node 0, zone   Normal"

static int buddyinfo_show(struct inode *inode, char *buf)
{
    unsigned long counts[CONFIG_MAX_ORDER];
    int len;

    (void)inode;
    buddy_free_areas(counts);
    len = proc_format(buf, BUDDYINFO_PREFIX);
    for (unsigned int order = 0; order < CONFIG_MAX_ORDER; ++order)
        len += proc_format(buf + len, " %lu", counts[order]);
    buf[len++] = '\n';
    return len;
}

static int buddyinfo_open(struct file *file)
{
    return proc_open(file,
            sizeof(BUDDYINFO_PREFIX) + CONFIG_MAX_ORDER
            * (1 + PROC_U64_DIGITS) + 1,
            buddyinfo_show);
}

const struct file_operations buddyinfo_fops = {
    .open = buddyinfo_open,
    .llseek = proc_llseek,
    .read = proc_read,
    .release = proc_release,
};

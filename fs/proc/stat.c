#include <nuvix/pid.h>
#include <nuvix/sched.h>
#include "proc.h"

#define STAT_FORMAT "ctxt %llu\nprocesses %llu\nprocs_running %u\n"

static int stat_show(struct inode *inode, char *buf)
{
    uint64_t switches;
    unsigned int runnable;

    (void)inode;
    sched_stats(&switches, &runnable);
    return proc_format(buf, STAT_FORMAT, (unsigned long long)switches,
                       (unsigned long long)pid_procs_created(), runnable);
}

static int stat_open(struct file *file)
{
    return proc_open(file,
            sizeof(STAT_FORMAT) + 3 * PROC_U64_DIGITS,
            stat_show);
}

const struct file_operations stat_fops = {
    .open = stat_open,
    .llseek = proc_llseek,
    .read = proc_read,
    .release = proc_release,
};

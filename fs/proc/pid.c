#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/pid.h>
#include <nuvix/sched.h>
#include <nuvix/string.h>
#include "proc.h"

#define STATUS_NAME   "status"
#define STATUS_FORMAT "Name:\t%s\nState:\t%s\nPid:\t%d\nPPid:\t%d\n"

static uint64_t proc_pid_ino(pid_t nr)
{
    return PID_MAX + 2UL * nr;
}

static pid_t proc_parse_pid(const char *name)
{
    pid_t nr = 0;

    if (*name < '1' || *name > '9')
        return 0;
    for (; *name; ++name) {
        if (*name < '0' || *name > '9')
            return 0;
        nr = 10 * nr + (*name - '0');
        if (nr >= PID_MAX)
            return 0;
    }
    return nr;
}

static bool proc_pid_valid(struct inode *inode)
{
    struct task *task __free(task_put) =
        pid_get_task(proc_i(inode)->pid, PIDTYPE_TGID);

    return task != NULL;
}

static bool proc_pid_revalidate(struct dentry *dentry)
{
    return proc_pid_valid(dentry->d_inode);
}

static const struct dentry_operations proc_pid_dops = {
    .d_revalidate = proc_pid_revalidate,
};

static const char *proc_task_state(struct task *task)
{
    struct rq *rq = task_rq(task);
    unsigned long flags;
    unsigned int state;

    spin_lock_irqsave(&rq->lock, flags);
    state = task->state;
    spin_unlock_irqrestore(&rq->lock, flags);

    switch (state) {
    case TASK_INTERRUPTIBLE:
        return "S (sleeping)";
    case TASK_UNINTERRUPTIBLE:
    case TASK_KILLABLE:
        return "D (disk sleep)";
    case TASK_STOPPED:
        return "T (stopped)";
    case TASK_ZOMBIE:
        return "Z (zombie)";
    default:
        return "R (running)";
    }
}

static void proc_task_name(struct task *task, char *out)
{
    spin_lock(&task->signal_lock);
    for (const char *name = task->name; *name; ++name) {
        if (*name == '\n' || *name == '\\')
            *out++ = '\\';
        *out++ = *name == '\n' ? 'n' : *name;
    }
    spin_unlock(&task->signal_lock);
    *out = '\0';
}

static int status_show(struct inode *inode, char *buf)
{
    struct pid *pid = proc_i(inode)->pid;
    struct task *task __free(task_put) = pid_get_task(pid, PIDTYPE_TGID);
    char name[2 * TASK_NAME_LEN];

    if (!task)
        return -ESRCH;
    proc_task_name(task, name);
    return proc_format(buf, STATUS_FORMAT, name, proc_task_state(task), pid->nr,
                       task_ppid_nr(task));
}

static int status_open(struct file *file)
{
    return proc_open(file,
                          sizeof(STATUS_FORMAT) + 2 * TASK_NAME_LEN +
                              sizeof("D (disk sleep)") + 2 * PROC_U64_DIGITS,
                          status_show);
}

static const struct file_operations status_fops = {
    .open = status_open,
    .llseek = proc_llseek,
    .read = proc_read,
    .release = proc_release,
};

static const struct inode_operations status_iops = {
    .getattr = proc_getattr,
};

static struct dentry *proc_pid_lookup(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode;

    if (!proc_pid_valid(dir))
        return ERR_PTR(-ENOENT);
    if (strcmp(dentry->d_name, STATUS_NAME))
        return dentry;
    inode = i_alloc(dir->i_sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    inode->i_ino = dir->i_ino + 1;
    inode->i_mode = S_IFREG | 0444;
    inode->i_nlink = 1;
    inode->i_ops = &status_iops;
    inode->i_fops = &status_fops;
    proc_i(inode)->pid = pid_get(proc_i(dir)->pid);
    dentry->d_inode = inode;
    dentry->d_ops = &proc_pid_dops;
    return dentry;
}

static int proc_pid_readdir(struct file *file, struct dirctx *ctx)
{
    struct inode *dir = file->f_inode;

    if (!proc_pid_valid(dir))
        return -ENOENT;
    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, 1, dir->i_ino, DT_DIR))
            return 0;
        ++ctx->pos;
    }
    if (ctx->pos == 1) {
        if (!dir_emit(ctx, "..", 2, 2, PROC_ROOT_INO, DT_DIR))
            return 0;
        ++ctx->pos;
    }
    if (ctx->pos == 2 && dir_emit(ctx, STATUS_NAME, sizeof(STATUS_NAME) - 1, 3,
                                  dir->i_ino + 1, DT_REG))
        ++ctx->pos;
    return 0;
}

static const struct file_operations proc_pid_fops = {
    .llseek = proc_llseek,
    .readdir = proc_pid_readdir,
};

static const struct inode_operations proc_pid_iops = {
    .lookup = proc_pid_lookup,
    .getattr = proc_getattr,
};

struct dentry *proc_lookup_pid(struct inode *dir, struct dentry *dentry)
{
    pid_t nr = proc_parse_pid(dentry->d_name);
    struct task *task __free(task_put) = NULL;
    struct inode *inode;

    if (!nr)
        return dentry;
    task = pid_lookup_task(nr, PIDTYPE_TGID);
    if (!task)
        return ERR_PTR(-ENOENT);
    inode = i_alloc(dir->i_sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    inode->i_ino = proc_pid_ino(nr);
    inode->i_mode = S_IFDIR | 0555;
    inode->i_nlink = 2;
    inode->i_ops = &proc_pid_iops;
    inode->i_fops = &proc_pid_fops;
    proc_i(inode)->pid = pid_get(task->pids[PIDTYPE_TGID]);
    dentry->d_inode = inode;
    dentry->d_ops = &proc_pid_dops;
    return dentry;
}

void proc_emit_pids(struct dirctx *ctx, loff_t base)
{
    while (ctx->pos < base + PID_MAX) {
        char name[PROC_U64_DIGITS + 1];
        pid_t nr = pid_next_proc(MAX(PID_INIT, ctx->pos - base));
        int len;

        if (nr == PID_MAX) {
            ctx->pos = base + PID_MAX;
            break;
        }
        len = proc_format(name, "%d", nr);
        if (!dir_emit(ctx, name, len, base + nr + 1, proc_pid_ino(nr), DT_DIR))
            break;
        ctx->pos = base + nr + 1;
    }
}

static ssize_t proc_self_readlink(struct inode *inode, char *buf, size_t size)
{
    pid_t nr = task_pid_nr(current, PIDTYPE_TGID);
    char name[PROC_U64_DIGITS + 1];
    size_t len;

    (void)inode;
    if (!nr)
        return -ENOENT;
    len = proc_format(name, "%d", nr);
    len = MIN(len, size);
    memcpy(buf, name, len);
    return len;
}

const struct inode_operations proc_self_iops = {
    .getattr = proc_getattr,
    .readlink = proc_self_readlink,
};

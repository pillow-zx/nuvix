#include <nuvix/pid.h>
#include <nuvix/task.h>
#include <nuvix/signal.h>
#include <nuvix/bitmap.h>
#include <nuvix/spinlock.h>
#include <nuvix/errno.h>
#include <nuvix/hashtable.h>
#include <nuvix/slab.h>
#include <nuvix/syscall.h>
#include <asm/trap.h>

static DECLARE_BITMAP(pidmap, PID_MAX) = {BIT_MASK(0)};
static unsigned int next_pid = PID_INIT;
static DEFINE_HASHTABLE(pid_table, 8);
static DEFINE_SPINLOCK(pid_lock);

struct pid *pid_alloc(void)
{
    struct pid *pid __free(kfree) = kzalloc(sizeof(*pid));
    unsigned long nr;

    if (!pid)
        return ERR_PTR(-ENOMEM);
    spin_lock(&pid_lock);
    nr = find_next_zero_bit(pidmap, PID_MAX, next_pid);
    if (nr == PID_MAX) {
        nr = find_next_zero_bit(pidmap, next_pid, PID_INIT);
        if (nr == next_pid) {
            spin_unlock(&pid_lock);
            return ERR_PTR(-EAGAIN);
        }
    }
    set_bit(nr, pidmap);
    next_pid = nr + 1;
    if (next_pid == PID_MAX)
        next_pid = PID_INIT;
    spin_unlock(&pid_lock);
    refcount_set(&pid->refcount, 1);
    pid->nr = nr;
    return move(pid);
}

void pid_put(struct pid *pid)
{
    if (!refcount_dec_and_test(&pid->refcount))
        return;
    Assert(hlist_unhashed(&pid->hash_node), "cannot free a published ID\n");
    spin_lock(&pid_lock);
    clear_bit(pid->nr, pidmap);
    spin_unlock(&pid_lock);
    kfree(pid);
}

static struct pid *find_pid_locked(pid_t nr)
{
    struct pid *pid;

    assert_spin_locked(&pid_lock);
    hash_for_each_possible (pid_table, pid, hash_node, nr) {
        if (pid->nr == nr)
            return pid;
    }
    return NULL;
}

static struct task *pid_task_locked(struct pid *pid, enum pid_type type)
{
    assert_spin_locked(&pid_lock);
    if (!pid)
        return NULL;
    return hlist_entry_safe(pid->tasks[type].first, struct task,
                            pid_links[type]);
}

struct task *pid_get_task(pid_t nr, enum pid_type type)
{
    struct task *task;
    spin_lock(&pid_lock);
    task = pid_task_locked(find_pid_locked(nr), type);
    if (task)
        task_get(task);
    spin_unlock(&pid_lock);

    return task;
}

static int signal_tasks(struct task **tasks, size_t count,
                        const struct siginfo *info)
{
    int ret = -ESRCH;

    for (size_t i = 0; i < count; ++i) {
        struct task *task __free(task_put) = tasks[i];
        int err = send_signal(task, info);

        if (ret)
            ret = err;
    }
    return ret;
}

static struct task *broadcast_task_locked(struct pid *pid)
{
    assert_spin_locked(&pid_lock);
    if (pid->nr == PID_INIT || pid == current->pids[PIDTYPE_TGID])
        return NULL;
    return pid_task_locked(pid, PIDTYPE_TGID);
}

int broadcast_signal(const struct siginfo *info)
{
    struct task **tasks __free(kfree) = NULL;
    struct pid *pid;
    size_t bucket;
    size_t count = 0, index = 0;
    int ret = -ESRCH;

    spin_lock(&pid_lock);
    hash_for_each (pid_table, bucket, pid, hash_node) {
        if (broadcast_task_locked(pid))
            ++count;
    }
    if (!count)
        goto out;
    if (!info->si_signo) {
        ret = 0;
        goto out;
    }
    tasks = kmalloc(count * sizeof(*tasks));
    if (!tasks) {
        ret = -ENOMEM;
        goto out;
    }
    hash_for_each (pid_table, bucket, pid, hash_node) {
        struct task *task = broadcast_task_locked(pid);

        if (!task)
            continue;
        task_get(task);
        tasks[index++] = task;
    }
    spin_unlock(&pid_lock);
    return signal_tasks(tasks, count, info);
out:
    spin_unlock(&pid_lock);
    return ret;
}

int send_group_signal(pid_t pgid, const struct siginfo *info)
{
    struct task **tasks __free(kfree) = NULL;
    struct pid *pgrp;
    struct task *task;
    size_t count = 0, index = 0;
    int ret = -ESRCH;

    spin_lock(&pid_lock);
    pgrp = pgid ? find_pid_locked(pgid) : current->pids[PIDTYPE_PGID];

    if (!pgrp)
        goto out;
    hlist_for_each_entry (task, &pgrp->tasks[PIDTYPE_PGID],
                          pid_links[PIDTYPE_PGID]) {
        ++count;
    }
    if (!count)
        goto out;
    if (!info->si_signo) {
        ret = 0;
        goto out;
    }
    tasks = kmalloc(count * sizeof(*tasks));
    if (!tasks) {
        ret = -ENOMEM;
        goto out;
    }
    hlist_for_each_entry (task, &pgrp->tasks[PIDTYPE_PGID],
                          pid_links[PIDTYPE_PGID]) {
        task_get(task);
        tasks[index++] = task;
    }
    spin_unlock(&pid_lock);
    return signal_tasks(tasks, count, info);
out:
    spin_unlock(&pid_lock);
    return ret;
}

pid_t task_pid_nr(const struct task *task, enum pid_type type)
{
    pid_t nr;

    spin_lock(&pid_lock);
    nr = task->pids[type] ? task->pids[type]->nr : 0;
    spin_unlock(&pid_lock);
    return nr;
}

int pid_prepare(struct task *task, struct task *parent)
{
    struct pid *pid __free(pid_put) = pid_alloc();

    if (IS_ERR(pid))
        return PTR_ERR(move(pid));
    task->pids[PIDTYPE_TID] = pid;
    task->pids[PIDTYPE_TGID] = pid;
    spin_lock(&pid_lock);
    task->pids[PIDTYPE_PGID] = parent ? parent->pids[PIDTYPE_PGID] : pid;
    task->pids[PIDTYPE_SID] = parent ? parent->pids[PIDTYPE_SID] : pid;
    for (enum pid_type type = 0; type < PIDTYPE_MAX; ++type)
        refcount_inc(&task->pids[type]->refcount);
    spin_unlock(&pid_lock);
    return 0;
}

void pid_publish(struct task *task)
{
    if (!task->parent)
        return;
    spin_lock(&pid_lock);
    Assert(hlist_empty(&task->pids[PIDTYPE_TID]->tasks[PIDTYPE_TID]),
           "TID already has a task\n");
    for (enum pid_type type = 0; type < PIDTYPE_MAX; ++type) {
        struct pid *pid = task->pids[type];

        Assert(hlist_unhashed(&task->pid_links[type]),
               "task identity already published\n");
        if (hlist_unhashed(&pid->hash_node))
            hash_add(pid_table, &pid->hash_node, pid->nr);
        hlist_add_head(&task->pid_links[type], &pid->tasks[type]);
    }
    spin_unlock(&pid_lock);
}

static void unhash_unused_pid(struct pid *pid)
{
    assert_spin_locked(&pid_lock);
    for (enum pid_type type = 0; type < PIDTYPE_MAX; ++type) {
        if (!hlist_empty(&pid->tasks[type]))
            return;
    }
    hash_del(&pid->hash_node);
}

void pid_unpublish(struct task *task)
{
    spin_lock(&pid_lock);
    Assert(!hlist_unhashed(&task->pid_links[PIDTYPE_TID]),
           "ID removal requires a published user task\n");
    for (enum pid_type type = 0; type < PIDTYPE_MAX; ++type) {
        hlist_del(&task->pid_links[type]);
        unhash_unused_pid(task->pids[type]);
    }
    spin_unlock(&pid_lock);
}

static void change_pid(struct task *task, enum pid_type type, struct pid *pid)
{
    struct pid *old = task->pids[type];

    assert_spin_locked(&pid_lock);
    hlist_del(&task->pid_links[type]);
    refcount_inc(&pid->refcount);
    task->pids[type] = pid;
    hlist_add_head(&task->pid_links[type], &pid->tasks[type]);
    unhash_unused_pid(old);
    refcount_dec(&old->refcount);
}

long sys_setpgid(struct trap_frame *frame)
{
    pid_t nr = (pid_t)syscallarg(frame, 0);
    pid_t pgid = (pid_t)syscallarg(frame, 1);
    struct pid *old __free(pid_put) = NULL;
    struct pid *tgid, *pgrp;
    struct task *task, *member;
    int ret;

    spin_lock(&pid_lock);
    if (!nr)
        nr = current->pids[PIDTYPE_TGID]->nr;
    if (!pgid)
        pgid = nr;
    ret = -EINVAL;
    if (pgid < 0)
        goto out;
    ret = -ESRCH;
    task = pid_task_locked(find_pid_locked(nr), PIDTYPE_TID);
    if (!task)
        goto out;
    tgid = task->pids[PIDTYPE_TGID];
    ret = -EINVAL;
    if (task->pids[PIDTYPE_TID] != tgid)
        goto out;
    if (task != current) {
        ret = -ESRCH;
        if (task->parent != current)
            goto out;
        ret = -EPERM;
        if (task->pids[PIDTYPE_SID] != current->pids[PIDTYPE_SID])
            goto out;
        ret = -EACCES;
        if (!task_test_flag(task, TF_FORKNOEXEC))
            goto out;
    }

    ret = -EPERM;
    if (task->pids[PIDTYPE_SID] == tgid)
        goto out;
    pgrp = find_pid_locked(pgid);
    if (pgrp != tgid) {
        member = pid_task_locked(pgrp, PIDTYPE_PGID);
        if (!member || member->pids[PIDTYPE_SID] != task->pids[PIDTYPE_SID])
            goto out;
    }
    ret = 0;
    old = task->pids[PIDTYPE_PGID];
    refcount_inc(&old->refcount);
    if (old == pgrp)
        goto out;
    hlist_for_each_entry (member, &tgid->tasks[PIDTYPE_TGID],
                          pid_links[PIDTYPE_TGID]) {
        change_pid(member, PIDTYPE_PGID, pgrp);
    }
out:
    spin_unlock(&pid_lock);
    return ret;
}

int pid_setsid(struct task *task)
{
    struct pid *old_pgrp __free(pid_put) = NULL;
    struct pid *old_sid __free(pid_put) = NULL;
    struct task *member;
    int ret = -EPERM;

    spin_lock(&pid_lock);
    struct pid *tgid = task->pids[PIDTYPE_TGID];

    if (task->pids[PIDTYPE_SID] == tgid ||
        !hlist_empty(&tgid->tasks[PIDTYPE_PGID]))
        goto out;
    old_pgrp = task->pids[PIDTYPE_PGID];
    old_sid = task->pids[PIDTYPE_SID];
    refcount_inc(&old_pgrp->refcount);
    refcount_inc(&old_sid->refcount);
    hlist_for_each_entry (member, &tgid->tasks[PIDTYPE_TGID],
                          pid_links[PIDTYPE_TGID]) {
        change_pid(member, PIDTYPE_PGID, tgid);
        change_pid(member, PIDTYPE_SID, tgid);
    }
    ret = tgid->nr;
out:
    spin_unlock(&pid_lock);
    return ret;
}

static long query_pid(pid_t nr, enum pid_type type)
{
    struct task *task;
    long ret = -ESRCH;

    spin_lock(&pid_lock);
    task = nr ? pid_task_locked(find_pid_locked(nr), PIDTYPE_TID) : current;
    if (task)
        ret = task->pids[type]->nr;
    spin_unlock(&pid_lock);
    return ret;
}

long sys_getpid(struct trap_frame *frame)
{
    (void)frame;
    return task_pid_nr(current, PIDTYPE_TGID);
}

long sys_gettid(struct trap_frame *frame)
{
    (void)frame;
    return task_pid_nr(current, PIDTYPE_TID);
}

long sys_getpgid(struct trap_frame *frame)
{
    return query_pid((pid_t)syscallarg(frame, 0), PIDTYPE_PGID);
}

long sys_getsid(struct trap_frame *frame)
{
    return query_pid((pid_t)syscallarg(frame, 0), PIDTYPE_SID);
}

long sys_setsid(struct trap_frame *frame)
{
    (void)frame;
    return pid_setsid(current);
}

#include <nuvix/pid.h>
#include <nuvix/bitmap.h>
#include <nuvix/spinlock.h>
#include <nuvix/errno.h>
#include <nuvix/hashtable.h>
#include <nuvix/slab.h>

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

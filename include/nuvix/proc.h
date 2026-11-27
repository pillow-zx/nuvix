#ifndef _NUVIX_PROC_H
#define _NUVIX_PROC_H

#include <nuvix/atomic.h>
#include <nuvix/list.h>
#include <nuvix/pid.h>
#include <nuvix/refcount.h>
#include <nuvix/signal.h>
#include <nuvix/spinlock.h>

struct proc {
    refcount_t refs;
    spinlock_t lock;
    struct list_head threads;
    atomic_t exiting;
    int status;
    struct pid *pids[PIDTYPE_MAX];

    struct sigpending pending;

    struct task *leader;
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
};

struct proc *proc_alloc(struct task *leader);

void proc_get(struct proc *proc);

void proc_put(struct proc *proc);

void proc_kill(struct proc *proc);

int proc_add_thread(struct task *task);

bool proc_remove_thread(struct task *task);

#define for_each_thread(proc, task)                                            \
    list_for_each_entry (task, &(proc)->threads, thread)

#endif

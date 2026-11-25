#ifndef _NUVIX_PROC_H
#define _NUVIX_PROC_H

#include <nuvix/atomic.h>
#include <nuvix/list.h>
#include <nuvix/pid.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>

struct proc {
    refcount_t refs;
    spinlock_t lock;
    struct list_head threads;
    atomic_t exiting;
    int status;
    struct pid *pids[PIDTYPE_MAX];

    struct task *leader;
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
};

struct proc *proc_alloc(void);

void proc_get(struct proc *proc);

void proc_put(struct proc *proc);

void proc_add_thread_locked(struct task *task);

bool proc_remove_thread(struct task *task);

#endif

#ifndef _NUVIX_PROC_H
#define _NUVIX_PROC_H

#include <nuvix/list.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>

enum proc_state {
    PROC_NORMAL,
    PROC_EXEC_PREPARING,
    PROC_EXEC_COMMITTING,
    PROC_EXITING,
    PROC_ZOMBIE,
    PROC_REAPED,
};

struct proc {
    refcount_t refcount;
    unsigned int tgid;

    spinlock_t state_lock;
    enum proc_state state;
    struct list_head members;
    unsigned int nr_live;

    /* Parent/child links will be protected by the process-tree lock. */
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
};

/*
 * After slub_init(); returns an unpublished, empty NORMAL proc with TGID 0
 * and one caller reference. Allocation failure returns NULL.
 */
struct proc *proc_alloc(void);
/* Requires a held reference or equivalent lifetime protection. */
void proc_get(struct proc *proc);
/* Final put currently supports only unpublished procs, outside short locks. */
void proc_put(struct proc *proc);

#endif /* _NUVIX_PROC_H */

#ifndef _NUVIX_PROC_H
#define _NUVIX_PROC_H

#include <nuvix/list.h>
#include <nuvix/refcount.h>
#include <nuvix/cleanup.h>
#include <nuvix/spinlock.h>
#include <nuvix/fdtable.h>

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

    /* Owned address space; prepared before task publication, then immutable. */
    struct mm_struct *mm;
    struct fdtable files;

    /* Parent/child links will be protected by the process-tree lock. */
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
};

/*
 * After slub_init(); returns an unpublished NORMAL proc with an empty mm/fdtable,
 * a reserved TGID and one caller reference. Allocation failure returns NULL.
 */
struct proc *proc_alloc(void);
/* Requires a held reference or equivalent lifetime protection. */
void proc_get(struct proc *proc);
/*
 * Final put releases mm, empty fdtable and TGID; supports only unpublished procs
 * outside short locks.
 */
void proc_put(struct proc *proc);

DECLARE_FREE(proc_put, struct proc *, if (_T) proc_put(_T));

#endif /* _NUVIX_PROC_H */

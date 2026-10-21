#ifndef _NUVIX_PID_H
#define _NUVIX_PID_H

#include <nuvix/cleanup.h>
#include <nuvix/hlist.h>
#include <nuvix/refcount.h>

typedef int pid_t;

#define PID_INIT 1
#define PID_MAX  32768

enum pid_type {
    PIDTYPE_TID,
    PIDTYPE_TGID,
    PIDTYPE_PGID,
    PIDTYPE_SID,
    PIDTYPE_MAX,
};

struct pid {
    refcount_t refcount;
    pid_t nr;
    struct hlist_node hash_node;
    struct hlist_head tasks[PIDTYPE_MAX];
};

struct task;
struct siginfo;

struct pid *pid_alloc(void);

void pid_put(struct pid *pid);

pid_t task_pid_nr(const struct task *task, enum pid_type type);

int pid_prepare(struct task *task, struct task *parent);

void pid_publish(struct task *task);

void pid_unpublish(struct task *task);

int pid_setsid(struct task *task);

DECLARE_FREE(pid_put, struct pid *, if (_T) pid_put(_T))

struct task *pid_get_task(pid_t nr, enum pid_type type);

int broadcast_signal(const struct siginfo *info);

int send_group_signal(pid_t pgid, const struct siginfo *info);

#endif /* _NUVIX_PID_H */

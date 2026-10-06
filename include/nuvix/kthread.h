#ifndef _NUVIX_KTHREAD_H
#define _NUVIX_KTHREAD_H

#include <nuvix/task.h>

/* Boot-only, after SMP and background thread dependencies are ready. */
int kthread_init(void);

/*
 * Same allocation contract as task_alloc(NULL, false), with a prepared first
 * entry. Publish with sched_publish() after all CPUs are ready. fn must be valid
 * while used; arg must remain valid while used by the thread. Returning exits
 * the thread; its result is not retained. The caller owns the returned reference.
 */
struct task *create_kthread(int (*fn)(void *), void *arg);

/*
 * Ordinary kernel thread context, no caller locks. Finish waits, timers and
 * asynchronous stack users before calling or returning from thread_fn.
 */
__noreturn void kthread_exit(void);

#endif /* _NUVIX_KTHREAD_H */

#ifndef _NUVIX_EXIT_H
#define _NUVIX_EXIT_H
#include <nuvix/proc.h>
#include <nuvix/types.h>
__noreturn void do_exit(int code);
__noreturn void do_exit_group(int code);
__noreturn void do_exit_signal(int sig);
int task_reaper_start(void);
void release_task(struct task_struct *task);
/* Results are consumed before userspace copyout; no copyout rollback. */
int kernel_wait4(pid_t pid, int options, struct proc_wait_info *result);
int kernel_waitid(pid_t pid, int options, struct proc_wait_info *result);
#endif

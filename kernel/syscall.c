#include <nuvix/syscall.h>
#include <nuvix/errno.h>
#include <nuvix/pid.h>
#include <nuvix/printk.h>
#include <asm/task.h>
#include <asm/trap.h>

#define __SYSCALL_ENTRY(name) [__NR_##name] = sys_##name,
static long (*const syscalls[__NR_syscalls])(struct trap_frame *) = {
    SYSCALL_LIST(__SYSCALL_ENTRY)};
#undef __SYSCALL_ENTRY

#ifdef CONFIG_TRACE_SYSCALL
#define __SYSCALL_NAME(name) [__NR_##name] = #name,
static const char *const syscall_names[__NR_syscalls] = {
    SYSCALL_LIST(__SYSCALL_NAME)};
#undef __SYSCALL_NAME

static inline void syscall_trace_enter(pid_t tid, unsigned long nr,
                                       const char *name,
                                       struct trap_frame *frame)
{
    pr_debug("[syscall] tid=%d enter nr=%lu name=%s "
             "args=[%#lx, %#lx, %#lx, %#lx, %#lx, %#lx]\n",
             tid, nr, name, syscallarg(frame, 0), syscallarg(frame, 1),
             syscallarg(frame, 2), syscallarg(frame, 3), syscallarg(frame, 4),
             syscallarg(frame, 5));
}

static inline void syscall_trace_leave(pid_t tid, unsigned long nr,
                                       const char *name, long ret)
{
    pr_debug("[syscall] tid=%d leave nr=%lu name=%s ret=%ld\n", tid, nr, name,
             ret);
}

#endif

long syscall_dispatch(unsigned long nr, struct trap_frame *frame)
{
    long ret;

#ifdef CONFIG_TRACE_SYSCALL
    const char *name = "unknown";
    pid_t tid = task_pid_nr(current, PIDTYPE_TID);

    if (nr < __NR_syscalls && syscall_names[nr])
        name = syscall_names[nr];

    syscall_trace_enter(tid, nr, name, frame);
#endif

    ret = nr >= __NR_syscalls || !syscalls[nr] ? -ENOSYS : syscalls[nr](frame);

#ifdef CONFIG_TRACE_SYSCALL
    syscall_trace_leave(tid, nr, name, ret);
#endif

    return ret;
}

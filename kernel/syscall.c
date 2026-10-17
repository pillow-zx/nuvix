#include <nuvix/syscall.h>
#include <nuvix/errno.h>

#define __SYSCALL_ENTRY(name) [__NR_##name] = sys_##name,
static long (*const syscalls[__NR_syscalls])(struct trap_frame *) = {
    SYSCALL_LIST(__SYSCALL_ENTRY)};
#undef __SYSCALL_ENTRY

long syscall_dispatch(unsigned long nr, struct trap_frame *frame)
{
    if (nr >= __NR_syscalls || !syscalls[nr])
        return -ENOSYS;
    return syscalls[nr](frame);
}

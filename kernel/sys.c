#include <nuvix/syscall.h>
#include <nuvix/errno.h>
#include <nuvix/uaccess.h>
#include <nuvix/sys.h>
#include <asm/trap.h>
#include <asm/sys.h>

long sys_uname(struct trap_frame *frame)
{
    struct utsname *name = (struct utsname *)syscallarg(frame, 0);
    static const struct utsname info = {
        .sysname = "Nuvix",
        .nodename = "nuvix",
        .release = "0.0.0",
        .version = "development",
        .machine = UTS_MACHINE,
        .domainname = "(none)",
    };

    if (copy_to_user(name, &info, sizeof(info)))
        return -EFAULT;
    return 0;
}

long sys_umask(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_getegid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_geteuid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_getuid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_getgid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_getresuid(struct trap_frame *frame)
{
    uint32_t *ruid = (uint32_t *)syscallarg(frame, 0);
    uint32_t *euid = (uint32_t *)syscallarg(frame, 1);
    uint32_t *suid = (uint32_t *)syscallarg(frame, 2);
    const uint32_t root = 0;

    if (copy_to_user(ruid, &root, sizeof(root)) ||
        copy_to_user(euid, &root, sizeof(root)) ||
        copy_to_user(suid, &root, sizeof(root)))
        return -EFAULT;
    return 0;
}

long sys_getresgid(struct trap_frame *frame)
{
    uint32_t *rgid = (uint32_t *)syscallarg(frame, 0);
    uint32_t *egid = (uint32_t *)syscallarg(frame, 1);
    uint32_t *sgid = (uint32_t *)syscallarg(frame, 2);
    const uint32_t root = 0;

    if (copy_to_user(rgid, &root, sizeof(root)) ||
        copy_to_user(egid, &root, sizeof(root)) ||
        copy_to_user(sgid, &root, sizeof(root)))
        return -EFAULT;
    return 0;
}

long sys_setuid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_setgid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_setreuid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_setregid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_setresuid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_setresgid(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

long sys_getgroups(struct trap_frame *frame)
{
    int size = (int)syscallarg(frame, 0);
    uint32_t *list = (uint32_t *)syscallarg(frame, 1);
    const uint32_t root = 0;

    if (size < 0)
        return -EINVAL;
    if (size && copy_to_user(list, &root, sizeof(root)))
        return -EFAULT;
    return 1;
}

long sys_setgroups(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}

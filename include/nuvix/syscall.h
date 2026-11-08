#ifndef _NUVIX_SYSCALL_H
#define _NUVIX_SYSCALL_H

#include <asm/syscall.h>

// clang-format off
#define SYSCALL_LIST(X)                                                        \
    X(openat) X(close) X(read) X(write) X(pipe2) X(readv) X(writev) X(setpgid) \
    X(getpgid) X(getsid) X(setsid) X(umask) X(getpid) X(getuid) X(geteuid)     \
    X(getgid) X(getegid) X(gettid) X(dup) X(dup3) X(fcntl) X(fchmodat)         \
    X(faccessat) X(exit) X(exit_group) X(brk) X(mmap) X(munmap) X(mprotect)    \
    X(kill) X(tkill) X(tgkill) X(rt_sigsuspend) X(rt_sigaction)                \
    X(rt_sigprocmask) X(rt_sigpending) X(rt_sigreturn) X(clone) X(wait4)       \
    X(execve) X(nanosleep) X(clock_gettime) X(clock_nanosleep) X(uname)        \
    X(getresuid) X(getresgid) X(setuid) X(setgid) X(setreuid) X(setregid)      \
    X(setresuid) X(setresgid) X(getgroups) X(setgroups) X(getppid) X(chdir)    \
    X(getcwd) X(newfstatat) X(fstat) X(getdents64) X(ioctl) X(lseek)           \
    X(set_tid_address) X(mkdirat) X(unlinkat) X(symlinkat) X(linkat)           \
    X(renameat2) X(utimensat) X(ftruncate) X(fchdir) X(pread64) X(pwrite64)    \
    X(ppoll) X(readlinkat) X(fsync) X(waitid) X(sigaltstack) X(reboot)         \
    X(mremap) X(getrandom)
// clang-format on

#ifndef __ASSEMBLER__

struct trap_frame;

#define __DECLARE_SYSCALL(name) long sys_##name(struct trap_frame *frame);
SYSCALL_LIST(__DECLARE_SYSCALL)
#undef __DECLARE_SYSCALL

long syscall_dispatch(unsigned long nr, struct trap_frame *frame);

#endif

#endif

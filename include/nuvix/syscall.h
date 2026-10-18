#ifndef _NUVIX_SYSCALL_H
#define _NUVIX_SYSCALL_H

#define __NR_getcwd          17
#define __NR_dup             23
#define __NR_dup3            24
#define __NR_fcntl           25
#define __NR_ioctl           29
#define __NR_mkdirat         34
#define __NR_unlinkat        35
#define __NR_symlinkat       36
#define __NR_linkat          37
#define __NR_ftruncate       46
#define __NR_chdir           49
#define __NR_fchdir          50
#define __NR_fchmodat        53
#define __NR_openat          56
#define __NR_close           57
#define __NR_pipe2           59
#define __NR_getdents64      61
#define __NR_lseek           62
#define __NR_read            63
#define __NR_write           64
#define __NR_readv           65
#define __NR_writev          66
#define __NR_pread64         67
#define __NR_pwrite64        68
#define __NR_readlinkat      78
#define __NR_newfstatat      79
#define __NR_fstat           80
#define __NR_fsync           82
#define __NR_exit            93
#define __NR_exit_group      94
#define __NR_set_tid_address 96
#define __NR_nanosleep       101
#define __NR_clock_gettime   113
#define __NR_clock_nanosleep 115
#define __NR_kill            129
#define __NR_tkill           130
#define __NR_tgkill          131
#define __NR_sigaltstack     132
#define __NR_rt_sigsuspend   133
#define __NR_rt_sigaction    134
#define __NR_rt_sigprocmask  135
#define __NR_rt_sigpending   136
#define __NR_rt_sigreturn    139
#define __NR_reboot          142
#define __NR_setpgid         154
#define __NR_getpgid         155
#define __NR_getsid          156
#define __NR_setsid          157
#define __NR_umask           166
#define __NR_getpid          172
#define __NR_getuid          174
#define __NR_geteuid         175
#define __NR_getgid          176
#define __NR_getegid         177
#define __NR_gettid          178
#define __NR_brk             214
#define __NR_munmap          215
#define __NR_clone           220
#define __NR_execve          221
#define __NR_mmap            222
#define __NR_mprotect        226
#define __NR_wait4           260
#define __NR_renameat2       276
#define __NR_syscalls        (__NR_renameat2 + 1)

// clang-format off
#define SYSCALL_LIST(X)                                                        \
    X(openat) X(close) X(read) X(write) X(readv) X(writev) X(setpgid)          \
    X(getpgid) X(getsid) X(setsid) X(umask) X(getpid) X(getuid) X(geteuid)     \
    X(getgid) X(getegid) X(gettid)
// clang-format on

struct trap_frame;

#define __DECLARE_SYSCALL(name) long sys_##name(struct trap_frame *frame);
SYSCALL_LIST(__DECLARE_SYSCALL)
#undef __DECLARE_SYSCALL

long syscall_dispatch(unsigned long nr, struct trap_frame *frame);

#endif

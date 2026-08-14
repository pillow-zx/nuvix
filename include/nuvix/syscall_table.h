#ifndef _NUVIX_SYSCALL_TABLE_H
#define _NUVIX_SYSCALL_TABLE_H

/**
 * @file syscall_table.h
 * @brief Installed Linux riscv64 syscall dispatch entries.
 */

#include <uapi/syscall.h>

/**
 * @def SYSCALL_TABLE
 * @brief X-macro list of syscalls wired into the dispatcher.
 * @param X Macro invoked as `X(number, name, handler)` for every entry.
 *
 * `number` is a SYS_* Linux riscv64 ABI constant, `name` is diagnostic text,
 * and `handler` is the kernel implementation installed in the dispatch table.
 * An entry in this list means the syscall has a handler, not that nuvix
 * implements complete Linux semantics; the maturity matrix lives in
 * SYSCALL.md.
 */
#define SYSCALL_TABLE(X)                                                       \
	X(SYS_getcwd, "getcwd", sys_getcwd)                                    \
	X(SYS_epoll_create1, "epoll_create1", sys_epoll_create1)               \
	X(SYS_epoll_ctl, "epoll_ctl", sys_epoll_ctl)                           \
	X(SYS_epoll_pwait, "epoll_pwait", sys_epoll_pwait)                     \
	X(SYS_dup, "dup", sys_dup)                                             \
	X(SYS_dup3, "dup3", sys_dup3)                                          \
	X(SYS_fcntl, "fcntl", sys_fcntl)                                       \
	X(SYS_ioctl, "ioctl", sys_ioctl)                                       \
	X(SYS_mknodat, "mknodat", sys_mknod)                                   \
	X(SYS_mkdirat, "mkdirat", sys_mkdirat)                                 \
	X(SYS_unlinkat, "unlinkat", sys_unlinkat)                              \
	X(SYS_symlinkat, "symlinkat", sys_symlinkat)                           \
	X(SYS_linkat, "linkat", sys_linkat)                                    \
	X(SYS_umount2, "umount2", sys_umount2)                                 \
	X(SYS_mount, "mount", sys_mount)                                       \
	X(SYS_statfs64, "statfs64", sys_statfs64)                              \
	X(SYS_fstatfs64, "fstatfs64", sys_fstatfs64)                           \
	X(SYS_ftruncate64, "ftruncate64", sys_ftruncate64)                     \
	X(SYS_fallocate, "fallocate", sys_fallocate)                           \
	X(SYS_faccessat, "faccessat", sys_faccessat)                           \
	X(SYS_chdir, "chdir", sys_chdir)                                       \
	X(SYS_fchmodat, "fchmodat", sys_fchmodat)                              \
	X(SYS_fchownat, "fchownat", sys_fchownat)                              \
	X(SYS_openat, "openat", sys_openat)                                    \
	X(SYS_close, "close", sys_close)                                       \
	X(SYS_pipe2, "pipe2", sys_pipe2)                                       \
	X(SYS_getdents64, "getdents64", sys_getdents64)                        \
	X(SYS_lseek, "lseek", sys_lseek)                                       \
	X(SYS_read, "read", sys_read)                                          \
	X(SYS_write, "write", sys_write)                                       \
	X(SYS_readv, "readv", sys_readv)                                       \
	X(SYS_writev, "writev", sys_writev)                                    \
	X(SYS_pread64, "pread64", sys_pread64)                                 \
	X(SYS_pwrite64, "pwrite64", sys_pwrite64)                              \
	X(SYS_sendfile, "sendfile", sys_sendfile)                              \
	X(SYS_pselect6, "pselect6", sys_pselect6)                              \
	X(SYS_ppoll, "ppoll", sys_ppoll)                                       \
	X(SYS_splice, "splice", sys_splice)                                    \
	X(SYS_readlinkat, "readlinkat", sys_readlinkat)                        \
	X(SYS_newfstatat, "newfstatat", sys_newfstatat)                        \
	X(SYS_fstat, "fstat", sys_fstat)                                       \
	X(SYS_sync, "sync", sys_sync)                                          \
	X(SYS_fsync, "fsync", sys_fsync)                                       \
	X(SYS_fdatasync, "fdatasync", sys_fdatasync)                           \
	X(SYS_utimensat, "utimensat", sys_utimensat)                           \
	X(SYS_exit, "exit", sys_exit)                                          \
	X(SYS_exit_group, "exit_group", sys_exit_group)                        \
	X(SYS_set_tid_addr, "set_tid_addr", sys_set_tid_addr)                  \
	X(SYS_futex, "futex", sys_futex)                                       \
	X(SYS_set_robust_list, "set_robust_list", sys_set_robust_list)         \
	X(SYS_get_robust_list, "get_robust_list", sys_get_robust_list)         \
	X(SYS_nanosleep, "nanosleep", sys_nanosleep)                           \
	X(SYS_getitimer, "getitimer", sys_getitimer)                           \
	X(SYS_setitimer, "setitimer", sys_setitimer)                           \
	X(SYS_timer_create, "timer_create", sys_timer_create)                  \
	X(SYS_timer_gettime, "timer_gettime", sys_timer_gettime)               \
	X(SYS_timer_getoverrun, "timer_getoverrun", sys_timer_getoverrun)      \
	X(SYS_timer_settime, "timer_settime", sys_timer_settime)               \
	X(SYS_timer_delete, "timer_delete", sys_timer_delete)                  \
	X(SYS_clock_settime, "clock_settime", sys_clock_settime)               \
	X(SYS_clock_gettime, "clock_gettime", sys_clock_gettime)               \
	X(SYS_clock_getres, "clock_getres", sys_clock_getres)                  \
	X(SYS_clock_nanosleep, "clock_nanosleep", sys_clock_nanosleep)         \
	X(SYS_syslog, "syslog", sys_syslog)                                    \
	X(SYS_sched_setaffinity, "sched_setaffinity", sys_sched_setaffinity)   \
	X(SYS_sched_getaffinity, "sched_getaffinity", sys_sched_getaffinity)   \
	X(SYS_sched_yield, "sched_yield", sys_sched_yield)                     \
	X(SYS_kill, "kill", sys_kill)                                          \
	X(SYS_tkill, "tkill", sys_tkill)                                       \
	X(SYS_tgkill, "tgkill", sys_tgkill)                                    \
	X(SYS_sigaltstack, "sigaltstack", sys_sigaltstack)                     \
	X(SYS_rt_sigsuspend, "rt_sigsuspend", sys_sigsuspend)                  \
	X(SYS_rt_sigaction, "rt_sigaction", sys_sigaction)                     \
	X(SYS_rt_sigprocmask, "rt_sigprocmask", sys_sigprocmask)               \
	X(SYS_rt_sigpending, "rt_sigpending", sys_sigpending)                  \
	X(SYS_rt_sigtimedwait, "rt_sigtimedwait", sys_sigtimedwait)            \
	X(SYS_rt_sigreturn, "rt_sigreturn", sys_sigreturn)                     \
	X(SYS_reboot, "reboot", sys_reboot)                                    \
	X(SYS_setgid, "setgid", sys_setgid)                                    \
	X(SYS_setuid, "setuid", sys_setuid)                                    \
	X(SYS_times, "times", sys_times)                                       \
	X(SYS_setpgid, "setpgid", sys_setpgid)                                 \
	X(SYS_getpgid, "getpgid", sys_getpgid)                                 \
	X(SYS_getsid, "getsid", sys_getsid)                                    \
	X(SYS_setsid, "setsid", sys_setsid)                                    \
	X(SYS_getgroups, "getgroups", sys_getgroups)                           \
	X(SYS_setgroups, "setgroups", sys_setgroups)                           \
	X(SYS_uname, "uname", sys_uname)                                       \
	X(SYS_getrusage, "getrusage", sys_getrusage)                           \
	X(SYS_umask, "umask", sys_umask)                                       \
	X(SYS_gettimeofday, "gettimeofday", sys_gettimeofday)                  \
	X(SYS_getpid, "getpid", sys_getpid)                                    \
	X(SYS_getppid, "getppid", sys_getppid)                                 \
	X(SYS_getuid, "getuid", sys_getuid)                                    \
	X(SYS_geteuid, "geteuid", sys_geteuid)                                 \
	X(SYS_getgid, "getgid", sys_getgid)                                    \
	X(SYS_getegid, "getegid", sys_getegid)                                 \
	X(SYS_gettid, "gettid", sys_gettid)                                    \
	X(SYS_sysinfo, "sysinfo", sys_sysinfo)                                 \
	X(SYS_brk, "brk", sys_brk)                                             \
	X(SYS_munmap, "munmap", sys_munmap)                                    \
	X(SYS_mremap, "mremap", sys_mremap)                                    \
	X(SYS_clone, "clone", sys_clone)                                       \
	X(SYS_execve, "execve", sys_execve)                                    \
	X(SYS_mmap, "mmap", sys_mmap)                                          \
	X(SYS_mprotect, "mprotect", sys_mprotect)                              \
	X(SYS_msync, "msync", sys_msync)                                       \
	X(SYS_mlock, "mlock", sys_mlock)                                       \
	X(SYS_munlock, "munlock", sys_munlock)                                 \
	X(SYS_mincore, "mincore", sys_mincore)                                 \
	X(SYS_madvise, "madvise", sys_madvise)                                 \
	X(SYS_wait4, "wait4", sys_wait4)                                       \
	X(SYS_prlimit64, "prlimit64", sys_prlimit64)                           \
	X(SYS_renameat2, "renameat2", sys_renameat2)                           \
	X(SYS_getrandom, "getrandom", sys_getrandom)                           \
	X(SYS_membarrier, "membarrier", sys_membarrier)                        \
	X(SYS_statx, "statx", sys_statx)                                       \
	X(SYS_rseq, "rseq", sys_rseq)                                          \
	X(SYS_faccessat2, "faccessat2", sys_faccessat2)                        \
	X(SYS_fchmodat2, "fchmodat2", sys_fchmodat2)

#endif

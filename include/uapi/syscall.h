#ifndef _NUVIX_UAPI_SYSCALL_H
#define _NUVIX_UAPI_SYSCALL_H

/**
 * @file syscall.h
 * @brief Linux riscv64 syscall numbers exported to kernel and userspace.
 *
 * The numeric values are ABI: userspace places the syscall number in RISC-V
 * register a7, syscall arguments in a0-a5, and the kernel returns the result
 * in a0. Successful calls return non-negative values; failures return Linux
 * negative errno values.
 */

#define SYS_getcwd	       17
#define SYS_epoll_create1      20
#define SYS_epoll_ctl	       21
#define SYS_epoll_pwait	       22
#define SYS_dup		       23
#define SYS_dup3	       24
#define SYS_fcntl	       25
#define SYS_ioctl	       29
#define SYS_mknodat	       33
#define SYS_mkdirat	       34
#define SYS_unlinkat	       35
#define SYS_symlinkat	       36
#define SYS_linkat	       37
#define SYS_umount2	       39
#define SYS_mount	       40
#define SYS_statfs64	       43
#define SYS_fstatfs64	       44
#define SYS_ftruncate64	       46
#define SYS_fallocate	       47
#define SYS_faccessat	       48
#define SYS_chdir	       49
#define SYS_fchmodat	       53
#define SYS_fchownat	       54
#define SYS_openat	       56
#define SYS_close	       57
#define SYS_pipe2	       59
#define SYS_getdents64	       61
#define SYS_lseek	       62
#define SYS_read	       63
#define SYS_write	       64
#define SYS_readv	       65
#define SYS_writev	       66
#define SYS_pread64	       67
#define SYS_pwrite64	       68
#define SYS_sendfile	       71
#define SYS_pselect6	       72
#define SYS_ppoll	       73
#define SYS_splice	       76
#define SYS_readlinkat	       78
#define SYS_newfstatat	       79
#define SYS_fstat	       80
#define SYS_sync	       81
#define SYS_fsync	       82
#define SYS_fdatasync	       83
#define SYS_utimensat	       88
#define SYS_exit	       93
#define SYS_exit_group	       94
#define SYS_waitid	       95
#define SYS_set_tid_addr       96
#define SYS_futex	       98
#define SYS_set_robust_list    99
#define SYS_get_robust_list    100
#define SYS_nanosleep	       101
#define SYS_getitimer	       102
#define SYS_setitimer	       103
#define SYS_timer_create       107
#define SYS_timer_gettime      108
#define SYS_timer_getoverrun   109
#define SYS_timer_settime      110
#define SYS_timer_delete       111
#define SYS_clock_settime      112
#define SYS_clock_gettime      113
#define SYS_clock_getres       114
#define SYS_clock_nanosleep    115
#define SYS_syslog	       116
#define SYS_sched_setaffinity  122
#define SYS_sched_getaffinity  123
#define SYS_sched_yield	       124
#define SYS_restart_syscall    128
#define SYS_kill	       129
#define SYS_tkill	       130
#define SYS_tgkill	       131
#define SYS_sigaltstack	       132
#define SYS_rt_sigsuspend      133
#define SYS_rt_sigaction       134
#define SYS_rt_sigprocmask     135
#define SYS_rt_sigpending      136
#define SYS_rt_sigtimedwait    137
#define SYS_rt_sigqueueinfo    138
#define SYS_rt_sigreturn       139
#define SYS_reboot	       142
#define SYS_setgid	       144
#define SYS_setuid	       146
#define SYS_times	       153
#define SYS_setpgid	       154
#define SYS_getpgid	       155
#define SYS_getsid	       156
#define SYS_setsid	       157
#define SYS_getgroups	       158
#define SYS_setgroups	       159
#define SYS_uname	       160
#define SYS_getrusage	       165
#define SYS_umask	       166
#define SYS_gettimeofday       169
#define SYS_getpid	       172
#define SYS_getppid	       173
#define SYS_getuid	       174
#define SYS_geteuid	       175
#define SYS_getgid	       176
#define SYS_getegid	       177
#define SYS_gettid	       178
#define SYS_sysinfo	       179
#define SYS_brk		       214
#define SYS_munmap	       215
#define SYS_mremap	       216
#define SYS_clone	       220
#define SYS_execve	       221
#define SYS_mmap	       222
#define SYS_mprotect	       226
#define SYS_msync	       227
#define SYS_mlock	       228
#define SYS_munlock	       229
#define SYS_mincore	       232
#define SYS_madvise	       233
#define SYS_riscv_hwprobe      258
#define SYS_riscv_flush_icache 259
#define SYS_wait4	       260
#define SYS_prlimit64	       261
#define SYS_process_vm_readv   270
#define SYS_process_vm_writev  271
#define SYS_renameat2	       276
#define SYS_getrandom	       278
#define SYS_membarrier	       283
#define SYS_statx	       291
#define SYS_rseq	       293
#define SYS_faccessat2	       439
#define SYS_fchmodat2	       452

/**
 * @def NR_SYSCALL
 * @brief Linux riscv64 syscall table size, including currently unsupported ABI
 * numbers.
 */
#define NR_SYSCALL 463

#ifndef __ASSEMBLER__
_Static_assert(SYS_getitimer == 102, "getitimer syscall number mismatch");
_Static_assert(SYS_setitimer == 103, "setitimer syscall number mismatch");
_Static_assert(SYS_timer_create == 107, "timer_create syscall number mismatch");
_Static_assert(SYS_timer_gettime == 108,
	       "timer_gettime syscall number mismatch");
_Static_assert(SYS_timer_getoverrun == 109,
	       "timer_getoverrun syscall number mismatch");
_Static_assert(SYS_timer_settime == 110,
	       "timer_settime syscall number mismatch");
_Static_assert(SYS_timer_delete == 111, "timer_delete syscall number mismatch");
_Static_assert(SYS_waitid == 95, "waitid syscall number mismatch");
_Static_assert(SYS_restart_syscall == 128, "restart_syscall number mismatch");
_Static_assert(SYS_rt_sigqueueinfo == 138,
	       "rt_sigqueueinfo syscall number mismatch");
_Static_assert(SYS_riscv_hwprobe == 258,
	       "riscv_hwprobe syscall number mismatch");
_Static_assert(SYS_riscv_flush_icache == 259,
	       "riscv_flush_icache syscall number mismatch");
_Static_assert(SYS_process_vm_readv == 270,
	       "process_vm_readv syscall number mismatch");
_Static_assert(SYS_process_vm_writev == 271,
	       "process_vm_writev syscall number mismatch");
_Static_assert(SYS_getsid == 156, "getsid syscall number mismatch");
_Static_assert(SYS_setsid == 157, "setsid syscall number mismatch");
#endif

#endif

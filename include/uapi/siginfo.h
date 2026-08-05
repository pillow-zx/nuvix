#ifndef _CUTEOS_UAPI_SIGINFO_H
#define _CUTEOS_UAPI_SIGINFO_H

#define SI_MAX_SIZE 128

#define SI_USER	    0
#define SI_KERNEL   0x80
#define SI_QUEUE    -1
#define SI_TIMER    -2
#define SI_MESGQ    -3
#define SI_ASYNCIO  -4
#define SI_SIGIO    -5
#define SI_TKILL    -6
#define SI_DETHREAD -7
#define SI_ASYNCNL  -60

#define SI_FROMUSER(siptr)   ((siptr)->si_code <= 0)
#define SI_FROMKERNEL(siptr) ((siptr)->si_code > 0)

#define ILL_ILLOPC   1
#define ILL_ILLOPN   2
#define ILL_ILLADR   3
#define ILL_ILLTRP   4
#define ILL_PRVOPC   5
#define ILL_PRVREG   6
#define ILL_COPROC   7
#define ILL_BADSTK   8
#define ILL_BADIADDR 9

#define FPE_INTDIV   1
#define FPE_INTOVF   2
#define FPE_FLTDIV   3
#define FPE_FLTOVF   4
#define FPE_FLTUND   5
#define FPE_FLTRES   6
#define FPE_FLTINV   7
#define FPE_FLTSUB   8
#define FPE_FLTUNK   14
#define FPE_CONDTRAP 15

#define SEGV_MAPERR  1
#define SEGV_ACCERR  2
#define SEGV_BNDERR  3
#define SEGV_PKUERR  4
#define SEGV_ACCADI  5
#define SEGV_ADIDERR 6
#define SEGV_ADIPERR 7
#define SEGV_MTEAERR 8
#define SEGV_MTESERR 9
#define SEGV_CPERR   10

#define BUS_ADRALN    1
#define BUS_ADRERR    2
#define BUS_OBJERR    3
#define BUS_MCEERR_AR 4
#define BUS_MCEERR_AO 5

#define TRAP_BRKPT	     1
#define TRAP_TRACE	     2
#define TRAP_BRANCH	     3
#define TRAP_HWBKPT	     4
#define TRAP_UNK	     5
#define TRAP_PERF	     6
#define TRAP_PERF_FLAG_ASYNC (1U << 0)

#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

#define POLL_IN	 1
#define POLL_OUT 2
#define POLL_MSG 3
#define POLL_ERR 4
#define POLL_PRI 5
#define POLL_HUP 6

#define SYS_SECCOMP	  1
#define SYS_USER_DISPATCH 2

union siginfo_fields {
	struct {
		int pid;
		unsigned int uid;
	} kill;
	struct {
		int tid;
		int overrun;
		union {
			int sival_int;
			void *sival_ptr;
		} sigval;
		int sys_private;
	} timer;
	struct {
		int pid;
		unsigned int uid;
		int status;
		long utime;
		long stime;
	} sigchld;
	struct {
		void *addr;
		int trapno;
	} sigfault;
};

typedef struct siginfo {
	union {
		struct {
			int si_signo;
			int si_errno;
			int si_code;
			int __pad0;
			union siginfo_fields fields;
		};
		int __pad[SI_MAX_SIZE / sizeof(int)];
	};
} siginfo_t;

#define si_pid	   fields.kill.pid
#define si_uid	   fields.kill.uid
#define si_tid	   fields.timer.tid
#define si_overrun fields.timer.overrun
#define si_value   fields.timer.sigval
#define si_int	   fields.timer.sigval.sival_int
#define si_ptr	   fields.timer.sigval.sival_ptr
#define si_status  fields.sigchld.status
#define si_utime   fields.sigchld.utime
#define si_stime   fields.sigchld.stime
#define si_addr	   fields.sigfault.addr
#define si_trapno  fields.sigfault.trapno

_Static_assert(sizeof(siginfo_t) == SI_MAX_SIZE,
	       "Linux siginfo_t size mismatch");

#endif

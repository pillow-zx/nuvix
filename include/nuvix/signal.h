#ifndef _NUVIX_SIGNAL_H
#define _NUVIX_SIGNAL_H

#include <nuvix/assert.h>
#include <nuvix/ctypes.h>
#include <nuvix/list.h>
#include <nuvix/pid.h>

#define SIGHUP       1
#define SIGINT       2
#define SIGQUIT      3
#define SIGILL       4
#define SIGTRAP      5
#define SIGABRT      6
#define SIGIOT       SIGABRT
#define SIGBUS       7
#define SIGFPE       8
#define SIGKILL      9
#define SIGUSR1      10
#define SIGSEGV      11
#define SIGUSR2      12
#define SIGPIPE      13
#define SIGALRM      14
#define SIGTERM      15
#define SIGSTKFLT    16
#define SIGCHLD      17
#define SIGCONT      18
#define SIGSTOP      19
#define SIGTSTP      20
#define SIGTTIN      21
#define SIGTTOU      22
#define SIGURG       23
#define SIGXCPU      24
#define SIGXFSZ      25
#define SIGVTALRM    26
#define SIGPROF      27
#define SIGWINCH     28
#define SIGIO        29
#define SIGPOLL      SIGIO
#define SIGPWR       30
#define SIGSYS       31
#define SIG_MAX      SIGSYS

#define SIGSET_SIZE  8
#define SIGINFO_SIZE 128

#define SA_SIGINFO   0x00000004UL
#define SA_ONSTACK   0x08000000UL
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL

#define SS_ONSTACK   1
#define SS_DISABLE   2

#define SI_USER      0
#define SI_KERNEL    0x80
#define SI_TKILL     (-6)

#define CLD_EXITED   1
#define CLD_KILLED   2

#define ILL_ILLOPC   1
#define ILL_ILLTRP   4
#define TRAP_BRKPT   1
#define BUS_ADRALN   1
#define BUS_ADRERR   2
#define SEGV_MAPERR  1
#define SEGV_ACCERR  2

typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);

typedef struct sigaltstack {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

#define SIG_DFL         ((sighandler_t)0)
#define SIG_IGN         ((sighandler_t)1)

#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

#define sigmask(sig)    (1UL << ((sig) - 1))
#define SIG_UNBLOCKMASK (sigmask(SIGKILL) | sigmask(SIGSTOP))
#define SIG_STOPMASK                                                           \
    (sigmask(SIGSTOP) | sigmask(SIGTSTP) | sigmask(SIGTTIN) | sigmask(SIGTTOU))

struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    sigset_t sa_mask;
};

typedef struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    int reserved;
    union {
        struct {
            pid_t pid;
            uint32_t uid;
        } kill;
        struct {
            pid_t pid;
            uint32_t uid;
            int status;
            long utime;
            long stime;
        } child;
        struct {
            void *addr;
        } fault;
        uint64_t padding[(SIGINFO_SIZE - 4 * sizeof(int)) / sizeof(uint64_t)];
    } fields;
} siginfo_t;

#define si_pid  fields.kill.pid
#define si_uid  fields.kill.uid
#define si_addr fields.fault.addr
#define si_status fields.child.status

struct ksignal {
    siginfo_t info;
    struct sigaction action;
    sigset_t oldmask;
};

struct sigqueue {
    struct list_head list;
    siginfo_t info;
};

struct sigpending {
    struct list_head list;
    sigset_t signal;
};

struct task;

#define sig_valid(task, sig)   (!(((task)->pending.signal) & sigmask((sig))))
#define sig_pending(task, sig) (!!(((task)->pending.signal) & sigmask((sig))))

void signal_task_init(struct task *task);
void signal_task_free(struct task *task);
void signal_clone(void);

int signal_stack_flags(unsigned long sp);
int signal_set_stack(const stack_t *stack, unsigned long sp);

bool get_signal(struct ksignal *signal, const siginfo_t *fault);
void signal_delivered(const struct ksignal *signal);

/* signal_lock held; pair every pending/mask/action change with this update. */
void recalc_sigpending(struct task *task);

/* Published user task held alive by caller; no PID, signal or rq locks held. */
int send_signal(struct task *task, const siginfo_t *info);

#endif

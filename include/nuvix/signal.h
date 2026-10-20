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
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL

#define SI_USER      0
#define SI_KERNEL    0x80
#define SI_TKILL     (-6)

typedef uint64_t sigset_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL         ((sighandler_t)0)
#define SIG_IGN         ((sighandler_t)1)

#define sigmask(sig)    (1UL << ((sig) - 1))
#define SIG_UNBLOCKABLE (sigmask(SIGKILL) | sigmask(SIGSTOP))

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
        uint64_t padding[(SIGINFO_SIZE - 4 * sizeof(int)) / sizeof(uint64_t)];
    } fields;
} siginfo_t;

#define si_pid fields.kill.pid
#define si_uid fields.kill.uid

struct sigqueue {
    struct list_head list;
    siginfo_t info;
};

struct sigpending {
    struct list_head list;
    sigset_t signal;
};

struct task;

void signal_task_init(struct task *task);
void signal_task_free(struct task *task);

#endif

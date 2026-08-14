#ifndef _NUVIX_UAPI_WAIT_H
#define _NUVIX_UAPI_WAIT_H

/**
 * @file wait.h
 * @brief Linux wait4(2) option constants.
 */

#define WNOHANG	    0x00000001
#define WUNTRACED   0x00000002
#define WSTOPPED    WUNTRACED
#define WEXITED	    0x00000004
#define WCONTINUED  0x00000008
#define WNOWAIT	    0x01000000
#define __WNOTHREAD 0x20000000
#define __WALL	    0x40000000
#define __WCLONE    0x80000000U

#define P_ALL	0
#define P_PID	1
#define P_PGID	2
#define P_PIDFD 3

_Static_assert(WNOHANG == 1, "WNOHANG ABI value mismatch");
_Static_assert(WUNTRACED == 2, "WUNTRACED ABI value mismatch");
_Static_assert(WCONTINUED == 8, "WCONTINUED ABI value mismatch");
_Static_assert(__WNOTHREAD == 0x20000000, "__WNOTHREAD ABI value mismatch");
_Static_assert(__WALL == 0x40000000, "__WALL ABI value mismatch");
_Static_assert(__WCLONE == 0x80000000U, "__WCLONE ABI value mismatch");

#endif

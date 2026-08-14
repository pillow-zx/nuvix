#ifndef _NUVIX_UAPI_EVENTPOLL_H
#define _NUVIX_UAPI_EVENTPOLL_H

/**
 * @file eventpoll.h
 * @brief Linux epoll UAPI constants and event layout.
 */

#include <uapi/fcntl.h>

#define EPOLL_CLOEXEC O_CLOEXEC

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN	       0x00000001U
#define EPOLLPRI       0x00000002U
#define EPOLLOUT       0x00000004U
#define EPOLLERR       0x00000008U
#define EPOLLHUP       0x00000010U
#define EPOLLNVAL      0x00000020U
#define EPOLLRDNORM    0x00000040U
#define EPOLLRDBAND    0x00000080U
#define EPOLLWRNORM    0x00000100U
#define EPOLLWRBAND    0x00000200U
#define EPOLLMSG       0x00000400U
#define EPOLLRDHUP     0x00002000U
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP    (1U << 29)
#define EPOLLONESHOT   (1U << 30)
#define EPOLLET	       (1U << 31)

/**
 * @struct epoll_event
 * @brief Event payload exchanged with epoll_ctl/epoll_pwait.
 *
 * @par Fields
 * - @c events: EPOLL* event mask.
 * - @c data: Userspace opaque payload.
 */
struct epoll_event {
	unsigned int events;
	unsigned long long data;
};

#undef offsetof
#define offsetof(t, d) __builtin_offsetof(t, d)

_Static_assert(sizeof(struct epoll_event) == 16,
	       "epoll_event ABI size mismatch");
_Static_assert(offsetof(struct epoll_event, data) == 8,
	       "epoll_event data ABI offset mismatch");
_Static_assert(EPOLLRDNORM == 0x40U, "EPOLLRDNORM ABI value mismatch");
_Static_assert(EPOLLWRNORM == 0x100U, "EPOLLWRNORM ABI value mismatch");

#endif

#ifndef _NUVIX_UAPI_POLL_H
#define _NUVIX_UAPI_POLL_H

/**
 * @file poll.h
 * @brief Linux poll/ppoll UAPI constants and pollfd layout.
 */

#define POLLIN	       0x0001
#define POLLPRI	       0x0002
#define POLLOUT	       0x0004
#define POLLERR	       0x0008
#define POLLHUP	       0x0010
#define POLLNVAL       0x0020
#define POLLRDNORM     0x0040
#define POLLRDBAND     0x0080
#define POLLWRNORM     0x0100
#define POLLWRBAND     0x0200
#define POLLMSG	       0x0400
#define POLLREMOVE     0x1000
#define POLLRDHUP      0x2000
#define POLLFREE       0x4000
#define POLL_BUSY_LOOP 0x8000

/**
 * @struct pollfd
 * @brief One file descriptor entry used by poll and ppoll.
 *
 * @par Fields
 * - @c fd: File descriptor number.
 * - @c events: Requested POLL* events.
 * - @c revents: Returned ready/error POLL* events.
 */
struct pollfd {
	int fd;
	short events;
	short revents;
};

#undef offsetof
#define offsetof(t, d) __builtin_offsetof(t, d)

_Static_assert(sizeof(struct pollfd) == 8, "pollfd ABI size mismatch");
_Static_assert(offsetof(struct pollfd, events) == 4,
	       "pollfd events ABI offset mismatch");
_Static_assert(offsetof(struct pollfd, revents) == 6,
	       "pollfd revents ABI offset mismatch");
_Static_assert(POLLRDNORM == 0x40, "POLLRDNORM ABI value mismatch");
_Static_assert(POLLWRNORM == 0x100, "POLLWRNORM ABI value mismatch");

#endif

#ifndef _NUVIX_POLL_H
#define _NUVIX_POLL_H

#include <nuvix/assert.h>
#include <nuvix/ctypes.h>

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int fd;
    short events;
    short revents;
};

static_assert(sizeof(struct pollfd) == 8, "invalid pollfd layout");

struct wait_queue;
struct poll_entry;

struct poll_table {
    struct poll_entry *entry;
};

void poll_add(struct wait_queue *queue, struct poll_table *table);

#endif /* ifndef _NUVIX_POLL_H */

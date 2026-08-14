#ifndef _NUVIX_EVENTPOLL_H
#define _NUVIX_EVENTPOLL_H

/**
 * @file eventpoll.h
 * @brief VFS-backed eventpoll file implementation.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <uapi/eventpoll.h>

struct file;
struct wait_deadline;

__must_check
struct file *eventpoll_file_alloc(void);

__must_check
bool eventpoll_file(struct file *file);

__must_check
int eventpoll_ctl(struct file *epfile, int op, int fd, struct file *file, const struct epoll_event *event);

__must_check
int eventpoll_wait(struct file *epfile, struct epoll_event *events, int maxevents, const struct wait_deadline *deadline);

#endif

#ifndef _NUVIX_EVENT_H
#define _NUVIX_EVENT_H

#include <nuvix/wait.h>

/* A source owns callback serialization. unsubscribe waits for any callback.
 * Callbacks run under the source lock, possibly in IRQ context: only publish
 * work and wake a task; never sleep, query another source, or unsubscribe. */
struct event_subscription {
	struct list_head node;
	struct wait_channel *source;
	void (*notify)(struct event_subscription *);
};

struct poll_table {
	int (*queue)(struct poll_table *, struct wait_channel *);
};

int poll_wait(struct poll_table *table, struct wait_channel *source);
struct file;
int vfs_poll_subscribe(struct file *file, uint32_t events,
		       struct poll_table *table);
void event_subscribe(struct event_subscription *sub,
		     struct wait_channel *source);
void event_unsubscribe(struct event_subscription *sub);

#endif

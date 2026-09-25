#ifndef _NUVIX_IO_H
#define _NUVIX_IO_H

#include <nuvix/types.h>
#include <nuvix/fs.h>
#include <nuvix/blkdev.h>
#include <nuvix/spinlock.h>

enum io_request_op {
	IO_REQUEST_READ,
	IO_REQUEST_WRITE,
	IO_REQUEST_FSYNC,
};

/* One operation's backend state. Frontends own file and data-buffer refs and
 * call io_request_release only after any submitted block callback has stopped
 * accessing the request. */
struct io_request {
	struct file *file;
	struct blk_request flush;
	struct wait_channel completion;
	/* Protects completion through the callback's final wake. */
	spinlock_t flush_lock;
	bool flush_done;
	void *buffer;
	size_t length;
	loff_t offset;
	int flush_result;
	int result;
	enum io_request_op op;
	unsigned int stage;
	bool datasync;
	bool position_locked;
	bool flush_submitted;
	bool irreversible;
	bool finished;
};

void io_request_init(struct io_request *req, struct file *file,
		enum io_request_op op, void *buffer, size_t length, loff_t offset, bool datasync);
ssize_t io_request_progress(struct io_request *req);
bool io_request_flush_done(struct io_request *req);

int io_request_cancel(struct io_request *req);

bool io_request_finish(struct io_request *req, int result);

void io_request_release(struct io_request *req);

struct wait_channel *io_position_channel(void);

bool io_position_trylock(void);

void io_position_unlock(void);

ssize_t io_sync_transfer(struct file *file, void *buffer, size_t length,
			loff_t *pos, bool write);

int io_sync_file(struct file *file, bool datasync);

ssize_t io_wait_transfer(struct file *file, void *buffer, size_t length, bool write);

#endif

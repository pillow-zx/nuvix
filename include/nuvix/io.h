#ifndef _NUVIX_IO_H
#define _NUVIX_IO_H
#include <nuvix/fs.h>

/* Synchronous completion frontend for native nonblocking file transfers.
 * Holds no source lock while sleeping; caller owns file and kernel buffer. */
ssize_t io_wait_transfer(struct file *file, void *buffer, size_t length,
			 bool write);
#endif

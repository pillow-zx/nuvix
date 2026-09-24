#ifndef _NUVIX_IO_H
#define _NUVIX_IO_H

#include <nuvix/types.h>
#include <nuvix/fs.h>

ssize_t io_wait_transfer(struct file *file, void *buffer, size_t length, bool write);

#endif

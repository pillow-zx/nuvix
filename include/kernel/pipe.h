#ifndef _CUTEOS_KERNEL_PIPE_H
#define _CUTEOS_KERNEL_PIPE_H

/*
 * include/kernel/pipe.h - 管道内部 API
 */

#include <kernel/types.h>
#include <kernel/page.h>

struct file;

/* Linux/POSIX atomic-write bound for one pipe write request. */
#define PIPE_BUF PAGE_SIZE

int do_pipe2(int fds[2], int flags);
bool pipe_file(struct file *file);
ssize_t pipe_splice_to_file(struct file *pipe_file, struct file *out_file,
			    loff_t *out_offset, size_t len);

#endif

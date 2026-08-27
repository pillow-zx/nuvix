#ifndef _NUVIX_PIPE_H
#define _NUVIX_PIPE_H

/*
 * include/nuvix/pipe.h - 管道内部 API
 */

#include <nuvix/types.h>
#include <nuvix/page.h>

struct file;
struct pipe_buffer;

struct pipe_consume_token {
	struct pipe_buffer *pipe;
	uint64_t generation;
	size_t length;
	bool active;
};

/* Linux/POSIX atomic-write bound for one pipe write request. */
#define PIPE_BUF PAGE_SIZE

int do_pipe2(int fds[2], int flags);
bool pipe_file(struct file *file);
ssize_t pipe_splice_to_file(struct file *pipe_file, struct file *out_file,
				    loff_t *out_offset, size_t len);
__must_check
int pipe_consume_begin(struct file *file, char *buffer, size_t count,
			       struct pipe_consume_token *token);
__must_check
int pipe_consume_commit(struct pipe_consume_token *token, size_t count);
void pipe_consume_abort(struct pipe_consume_token *token);

#endif

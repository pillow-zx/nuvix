#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#define IO_PIPE_RECORDS 4

static volatile sig_atomic_t io_interrupted;

static void io_signal_handler(int signal)
{
	(void)signal;
	io_interrupted = 1;
}

static void io_fill_pipe(int fd, char value)
{
	char buffer[PIPE_BUF];

	memset(buffer, value, sizeof(buffer));
	UT_ASSERT_EQ(write(fd, buffer, sizeof(buffer)),
		     (ssize_t)sizeof(buffer));
}

static void io_write_pipe_records(int fd, char value, int vector)
{
	char record[PIPE_BUF];
	struct iovec iov[] = {
		{.iov_base = record, .iov_len = sizeof(record) / 2},
		{.iov_base = record + sizeof(record) / 2,
		 .iov_len = sizeof(record) - sizeof(record) / 2},
	};

	memset(record, value, sizeof(record));
	for (int i = 0; i < IO_PIPE_RECORDS; i++) {
		ssize_t ret;

		if (vector)
			ret = writev(fd, iov, sizeof(iov) / sizeof(iov[0]));
		else
			ret = write(fd, record, sizeof(record));

		if (ret != (ssize_t)sizeof(record))
			_exit(1);
	}

	_exit(0);
}

UT_CASE(io_pipe_vector_and_offset, 1500)
{
	char first[4] = {};
	char second[8] = {};
	char read_buffer[8] = {};
	struct iovec write_iov[] = {
		{.iov_base = "abc", .iov_len = 3},
		{.iov_base = "def", .iov_len = 3},
	};
	struct iovec read_iov[] = {
		{.iov_base = first, .iov_len = 3},
		{.iov_base = second, .iov_len = 3},
	};
	int pipefd[2];
	int fd;

	UT_ASSERT_EQ(pipe(pipefd), 0);
	UT_ASSERT_EQ(writev(pipefd[1], write_iov, 2), 6);
	UT_ASSERT_EQ(readv(pipefd[0], read_iov, 2), 6);
	UT_EXPECT_STREQ(first, "abc");
	UT_EXPECT_STREQ(second, "def");
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(ut_write_file("offset", "abcdef", 6, 0600), 0);
	fd = open("offset", O_RDWR);
	UT_ASSERT(fd >= 0);
	UT_ASSERT_EQ(pwrite(fd, "XY", 2, 2), 2);
	UT_ASSERT_EQ(pread(fd, read_buffer, 6, 0), 6);
	UT_EXPECT_MEMEQ(read_buffer, "abXYef", 6);
	UT_EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 0);
	UT_ASSERT_EQ(close(fd), 0);
}

UT_CASE(io_nonblocking_and_sigpipe, 1500)
{
	char byte;
	int pipefd[2];
	int ready_pipe[2];
	int start_pipe[2];
	pid_t child;

	UT_ASSERT_EQ(pipe2(pipefd, O_NONBLOCK), 0);
	UT_EXPECT_ERRNO(read(pipefd[0], &(char){0}, 1), EAGAIN);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(pipe(pipefd), 0);
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	UT_ASSERT_EQ(pipe(start_pipe), 0);
	child = UT_FORK();
	if (child == 0) {
		(void)close(pipefd[0]);
		(void)close(ready_pipe[0]);
		(void)close(start_pipe[1]);
		if (write(ready_pipe[1], "r", 1) != 1 ||
		    read(start_pipe[0], &byte, 1) != 0)
			_exit(127);
		(void)write(pipefd[1], "x", 1);
		_exit(127);
	}
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(close(ready_pipe[1]), 0);
	UT_ASSERT_EQ(close(start_pipe[0]), 0);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	UT_ASSERT_EQ(close(start_pipe[1]), 0);
	UT_EXPECT_SIGNAL(child, SIGPIPE);
	UT_ASSERT_EQ(close(ready_pipe[0]), 0);
}

UT_CASE(io_pipe_buf_atomicity, 5000)
{
	char output[PIPE_BUF];
	char byte;
	int data_pipe[2];
	int ready_pipe[2];
	int start_pipe[2];
	int records_a = 0;
	int records_b = 0;
	pid_t writer_a;
	pid_t writer_b;

	UT_ASSERT_EQ(pipe(data_pipe), 0);
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	UT_ASSERT_EQ(pipe(start_pipe), 0);
	writer_a = UT_FORK();
	if (writer_a == 0) {
		(void)close(data_pipe[0]);
		(void)close(ready_pipe[0]);
		(void)close(start_pipe[1]);
		if (write(ready_pipe[1], "a", 1) != 1 ||
		    read(start_pipe[0], &byte, 1) != 0)
			_exit(1);
		io_write_pipe_records(data_pipe[1], 'A', 0);
	}
	writer_b = UT_FORK();
	if (writer_b == 0) {
		(void)close(data_pipe[0]);
		(void)close(ready_pipe[0]);
		(void)close(start_pipe[1]);
		if (write(ready_pipe[1], "b", 1) != 1 ||
		    read(start_pipe[0], &byte, 1) != 0)
			_exit(1);
		io_write_pipe_records(data_pipe[1], 'B', 1);
	}

	UT_ASSERT_EQ(close(data_pipe[1]), 0);
	UT_ASSERT_EQ(close(ready_pipe[1]), 0);
	UT_ASSERT_EQ(close(start_pipe[0]), 0);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	UT_ASSERT_EQ(close(start_pipe[1]), 0);
	for (int i = 0; i < 2 * IO_PIPE_RECORDS; i++) {
		int record_a = 1;
		int record_b = 1;

		UT_ASSERT_EQ(read(data_pipe[0], output, sizeof(output)),
			     (ssize_t)sizeof(output));
		for (size_t j = 0; j < sizeof(output); j++) {
			if (output[j] != 'A')
				record_a = 0;
			if (output[j] != 'B')
				record_b = 0;
		}
		UT_EXPECT(record_a || record_b);
		if (record_a)
			records_a++;
		if (record_b)
			records_b++;
	}
	UT_EXPECT_EQ(records_a, IO_PIPE_RECORDS);
	UT_EXPECT_EQ(records_b, IO_PIPE_RECORDS);
	UT_EXPECT_EXIT(writer_a, 0);
	UT_EXPECT_EXIT(writer_b, 0);
	UT_ASSERT_EQ(close(ready_pipe[0]), 0);
	UT_ASSERT_EQ(close(data_pipe[0]), 0);
}

UT_CASE(io_pipe_last_reader_wakes_writer, 5000)
{
	struct sigaction ignore = {
		.sa_handler = SIG_IGN,
	};
	char byte;
	int data_pipe[2];
	int ready_pipe[2];
	pid_t child;

	UT_ASSERT_EQ(pipe(data_pipe), 0);
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	io_fill_pipe(data_pipe[1], 'f');
	child = UT_FORK();
	if (child == 0) {
		(void)close(data_pipe[0]);
		(void)close(ready_pipe[0]);
		if (sigaction(SIGPIPE, &ignore, NULL) != 0 ||
		    write(ready_pipe[1], "w", 1) != 1)
			_exit(1);
		errno = 0;
		if (write(data_pipe[1], "w", 1) != -1 || errno != EPIPE)
			_exit(1);
		_exit(0);
	}
	UT_ASSERT_EQ(close(data_pipe[1]), 0);
	UT_ASSERT_EQ(close(ready_pipe[1]), 0);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	UT_ASSERT_EQ(nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL),
		     0);
	UT_ASSERT_EQ(close(data_pipe[0]), 0);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(ready_pipe[0]), 0);
}

UT_CASE(io_pipe_last_writer_wakes_reader, 5000)
{
	char byte;
	int data_pipe[2];
	int ready_pipe[2];
	pid_t child;

	UT_ASSERT_EQ(pipe(data_pipe), 0);
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	child = UT_FORK();
	if (child == 0) {
		(void)close(data_pipe[1]);
		(void)close(ready_pipe[0]);
		if (write(ready_pipe[1], "r", 1) != 1 ||
		    read(data_pipe[0], &byte, 1) != 0)
			_exit(1);
		_exit(0);
	}
	UT_ASSERT_EQ(close(data_pipe[0]), 0);
	UT_ASSERT_EQ(close(ready_pipe[1]), 0);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	UT_ASSERT_EQ(nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL),
		     0);
	UT_ASSERT_EQ(close(data_pipe[1]), 0);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(ready_pipe[0]), 0);
}

UT_CASE(io_pipe_nonblocking_and_endpoint_lifecycle, 5000)
{
	struct sigaction ignore = {
		.sa_handler = SIG_IGN,
	};
	struct sigaction old_action;
	char buffer[PIPE_BUF + 1];
	char byte;
	int data_pipe[2];
	int ready_pipe[2];
	int pipefd[2];
	pid_t child;
	ssize_t count;

	memset(buffer, 'p', sizeof(buffer));
	UT_ASSERT_EQ(pipe2(pipefd, O_NONBLOCK), 0);
	io_fill_pipe(pipefd[1], 'f');
	UT_EXPECT_ERRNO(write(pipefd[1], "x", 1), EAGAIN);
	UT_ASSERT_EQ(read(pipefd[0], &byte, 1), 1);
	UT_EXPECT_ERRNO(write(pipefd[1], "yz", 2), EAGAIN);
	UT_ASSERT_EQ(read(pipefd[0], buffer, PIPE_BUF - 1), PIPE_BUF - 1);
	io_fill_pipe(pipefd[1], 'f');
	UT_ASSERT_EQ(read(pipefd[0], buffer, 128), 128);
	count = write(pipefd[1], buffer, sizeof(buffer));
	UT_EXPECT(count > 0 && count <= 128);
	UT_ASSERT_EQ(read(pipefd[0], buffer, 128), 128);
	count = writev(
		pipefd[1],
		(struct iovec[]){{.iov_base = buffer, .iov_len = PIPE_BUF},
				 {.iov_base = buffer, .iov_len = 1}},
		2);
	UT_EXPECT(count > 0 && count <= 128);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(pipe(pipefd), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_EXPECT_EQ(read(pipefd[0], &byte, 1), 0);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(sigaction(SIGPIPE, &ignore, &old_action), 0);
	UT_ASSERT_EQ(pipe(pipefd), 0);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_EXPECT_ERRNO(write(pipefd[1], "x", 1), EPIPE);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(sigaction(SIGPIPE, &old_action, NULL), 0);
	UT_ASSERT_EQ(pipe(data_pipe), 0);
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	io_fill_pipe(data_pipe[1], 'f');
	child = UT_FORK();
	if (child == 0) {
		(void)close(data_pipe[0]);
		(void)close(ready_pipe[0]);
		if (write(ready_pipe[1], "r", 1) != 1 ||
		    write(data_pipe[1], "w", 1) != 1)
			_exit(1);
		_exit(0);
	}
	UT_ASSERT_EQ(close(data_pipe[1]), 0);
	UT_ASSERT_EQ(close(ready_pipe[1]), 0);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	UT_ASSERT_EQ(nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL),
		     0);
	UT_ASSERT_EQ(read(data_pipe[0], &byte, 1), 1);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(ready_pipe[0]), 0);
	UT_ASSERT_EQ(close(data_pipe[0]), 0);
}

UT_CASE(io_pipe_fd_lifecycle, 1500)
{
	char byte;
	char *path;
	int file_dup;
	int file_fd;
	int pipefd[2];
	int read_dup;
	int fcntl_dup;
	int replaced;

	UT_ASSERT_EQ(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK), 0);
	UT_EXPECT_EQ(fcntl(pipefd[0], F_GETFD), FD_CLOEXEC);
	UT_EXPECT_EQ(fcntl(pipefd[1], F_GETFD), FD_CLOEXEC);
	UT_EXPECT(fcntl(pipefd[0], F_GETFL) & O_NONBLOCK);
	UT_EXPECT(fcntl(pipefd[1], F_GETFL) & O_NONBLOCK);

	read_dup = dup(pipefd[0]);
	UT_ASSERT(read_dup >= 0);
	UT_EXPECT_EQ(fcntl(read_dup, F_GETFD), 0);
	UT_ASSERT_EQ(fcntl(read_dup, F_SETFD, FD_CLOEXEC), 0);
	UT_ASSERT_EQ(fcntl(pipefd[0], F_SETFD, 0), 0);
	UT_EXPECT_EQ(fcntl(read_dup, F_GETFD), FD_CLOEXEC);
	UT_EXPECT_EQ(fcntl(pipefd[0], F_GETFD), 0);
	UT_EXPECT(fcntl(read_dup, F_GETFL) & O_NONBLOCK);
	UT_ASSERT_EQ(fcntl(read_dup, F_SETFL,
			   fcntl(read_dup, F_GETFL) & ~O_NONBLOCK),
		     0);
	UT_EXPECT(!(fcntl(pipefd[0], F_GETFL) & O_NONBLOCK));
	UT_ASSERT_EQ(fcntl(pipefd[0], F_SETFL,
			   fcntl(pipefd[0], F_GETFL) | O_NONBLOCK),
		     0);
	UT_EXPECT(fcntl(read_dup, F_GETFL) & O_NONBLOCK);

	fcntl_dup = fcntl(read_dup, F_DUPFD_CLOEXEC, 0);
	UT_ASSERT(fcntl_dup >= 0);
	UT_EXPECT_EQ(fcntl(fcntl_dup, F_GETFD), FD_CLOEXEC);
	UT_EXPECT(fcntl(fcntl_dup, F_GETFL) & O_NONBLOCK);
	UT_ASSERT_EQ(close(fcntl_dup), 0);

	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(write(pipefd[1], "x", 1), 1);
	UT_ASSERT_EQ(read(read_dup, &byte, 1), 1);
	UT_EXPECT_EQ(byte, 'x');

	replaced = dup(pipefd[1]);
	UT_ASSERT(replaced >= 0);
	UT_ASSERT_EQ(dup3(read_dup, replaced, O_CLOEXEC), replaced);
	UT_EXPECT_EQ(fcntl(replaced, F_GETFD), FD_CLOEXEC);
	UT_EXPECT_ERRNO(dup3(read_dup, read_dup, 0), EINVAL);
	UT_EXPECT_ERRNO(dup3(read_dup, replaced, O_NONBLOCK), EINVAL);
	UT_ASSERT_EQ(close(replaced), 0);
	UT_ASSERT_EQ(close(read_dup), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);

	path = ut_path("shared-status-flags");
	UT_ASSERT(path != NULL);
	file_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	UT_ASSERT(file_fd >= 0);
	file_dup = dup(file_fd);
	UT_ASSERT(file_dup >= 0);
	UT_ASSERT_EQ(
		fcntl(file_dup, F_SETFL, fcntl(file_dup, F_GETFL) | O_APPEND),
		0);
	UT_EXPECT(fcntl(file_fd, F_GETFL) & O_APPEND);
	UT_ASSERT_EQ(close(file_dup), 0);
	UT_ASSERT_EQ(close(file_fd), 0);
	free(path);
}

UT_CASE(io_unsupported_file_controls, 1500)
{
	char *path;
	int file_fd;
	int pipefd[2];

	UT_ASSERT_EQ(pipe(pipefd), 0);
	UT_EXPECT_ERRNO(fcntl(pipefd[0], F_SETPIPE_SZ, PIPE_BUF), EINVAL);
	UT_EXPECT_ERRNO(fcntl(pipefd[0], F_GETPIPE_SZ), EINVAL);
	UT_EXPECT_ERRNO(fcntl(pipefd[0], F_SETLK, 0), EINVAL);
	UT_EXPECT_ERRNO(fcntl(-1, F_GETPIPE_SZ), EBADF);
	path = ut_path("unsupported-file-controls");
	UT_ASSERT(path != NULL);
	file_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	UT_ASSERT(file_fd >= 0);
	UT_EXPECT_ERRNO(fallocate(file_fd, FALLOC_FL_KEEP_SIZE, 0, 1), EINVAL);
	UT_EXPECT_ERRNO(
		splice(pipefd[0], NULL, file_fd, NULL, 0, SPLICE_F_NONBLOCK),
		EINVAL);
	UT_ASSERT_EQ(close(file_fd), 0);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	free(path);
}

UT_CASE(io_sendfile_and_splice, 1500)
{
	char *content;
	int input;
	int output;
	int pipefd[2];

	UT_ASSERT_EQ(ut_write_file("input", "sendfile-data", 13, 0600), 0);
	input = open("input", O_RDONLY);
	output = open("output", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	UT_ASSERT(input >= 0);
	UT_ASSERT(output >= 0);
	UT_ASSERT_EQ(sendfile(output, input, NULL, 13), 13);
	UT_ASSERT_EQ(close(input), 0);
	UT_ASSERT_EQ(close(output), 0);
	content = ut_read_file("output", NULL);
	UT_ASSERT(content != NULL);
	UT_EXPECT_STREQ(content, "sendfile-data");
	free(content);
	UT_ASSERT_EQ(pipe(pipefd), 0);
	UT_ASSERT_EQ(write(pipefd[1], "splice-data", 11), 11);
	output = open("spliced", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	UT_ASSERT(output >= 0);
	UT_ASSERT_EQ(splice(pipefd[0], NULL, output, NULL, 11, 0), 11);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(close(output), 0);
	content = ut_read_file("spliced", NULL);
	UT_ASSERT(content != NULL);
	UT_EXPECT_STREQ(content, "splice-data");
	free(content);
	input = open("input", O_RDONLY);
	output = open("output", O_WRONLY);
	UT_ASSERT(input >= 0);
	UT_ASSERT(output >= 0);
	UT_EXPECT_ERRNO(splice(input, NULL, output, NULL, 1, 0), EINVAL);
	UT_ASSERT_EQ(close(input), 0);
	UT_ASSERT_EQ(close(output), 0);
}

UT_CASE(io_poll_select_epoll_and_eintr, 5000)
{
	struct sigaction action = {
		.sa_handler = io_signal_handler,
	};
	struct sigaction old_action;
	struct pollfd pollfd;
	struct epoll_event event = {
		.events = EPOLLIN,
		.data.u32 = 17,
	};
	struct epoll_event received = {};
	struct timespec timeout = {
		.tv_nsec = 500000000,
	};
	fd_set readfds;
	int pipefd[2];
	int epollfd;
	pid_t child;

	UT_ASSERT_EQ(pipe(pipefd), 0);
	pollfd = (struct pollfd){
		.fd = pipefd[0],
		.events = POLLIN,
	};
	UT_EXPECT_EQ(poll(&pollfd, 1, 0), 0);
	UT_ASSERT_EQ(write(pipefd[1], "p", 1), 1);
	UT_ASSERT_EQ(poll(&pollfd, 1, 0), 1);
	UT_EXPECT(pollfd.revents & POLLIN);
	FD_ZERO(&readfds);
	FD_SET(pipefd[0], &readfds);
	UT_ASSERT_EQ(pselect(pipefd[0] + 1, &readfds, NULL, NULL,
			     &(struct timespec){0}, NULL),
		     1);
	UT_EXPECT(FD_ISSET(pipefd[0], &readfds));
	epollfd = epoll_create1(EPOLL_CLOEXEC);
	UT_ASSERT(epollfd >= 0);
	UT_ASSERT_EQ(epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &event), 0);
	UT_ASSERT_EQ(epoll_pwait(epollfd, &received, 1, 0, NULL), 1);
	UT_EXPECT_EQ(received.data.u32, 17);
	UT_ASSERT_EQ(read(pipefd[0], &(char){0}, 1), 1);
	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, &old_action), 0);
	io_interrupted = 0;
	child = UT_FORK();
	if (child == 0) {
		(void)nanosleep(&(struct timespec){.tv_nsec = 50000000}, NULL);
		(void)kill(getppid(), SIGUSR1);
		_exit(0);
	}
	pollfd.revents = 0;
	UT_EXPECT_ERRNO(ppoll(&pollfd, 1, &timeout, NULL), EINTR);
	UT_EXPECT(io_interrupted);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &old_action, NULL), 0);
	UT_ASSERT_EQ(close(epollfd), 0);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
}

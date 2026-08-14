#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <utest.h>

UT_CASE(mm_brk_growth_public_musl_xfail, 1500)
{
	void *initial = sbrk(0);
	void *grown;

	UT_ASSERT(initial != (void *)-1);
	UT_XFAIL("musl intentionally exposes only sbrk(0), not brk growth");
	grown = sbrk(4096);
	UT_EXPECT_NE(grown, (void *)-1);
	UT_EXPECT((uintptr_t)sbrk(0) >= (uintptr_t)initial + 4096);
}

UT_CASE(mm_anonymous_protection_unmap_and_mremap, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *mapping;
	unsigned char *resized;
	pid_t child;

	UT_ASSERT(page_size > 0);
	mapping = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(mapping != MAP_FAILED);
	UT_EXPECT_EQ(mapping[0], 0);
	mapping[0] = 1;
	mapping[page_size * 2] = 3;
	UT_ASSERT_EQ(mprotect(mapping, page_size, PROT_READ), 0);
	child = UT_FORK();
	if (child == 0) {
		mapping[0] = 2;
		_exit(127);
	}
	UT_EXPECT_SIGNAL(child, SIGSEGV);
	UT_ASSERT_EQ(mprotect(mapping, page_size, PROT_READ | PROT_WRITE), 0);
	UT_ASSERT_EQ(munmap(mapping + page_size, page_size), 0);
	child = UT_FORK();
	if (child == 0) {
		volatile unsigned char value = mapping[page_size];

		(void)value;
		_exit(127);
	}
	UT_EXPECT_SIGNAL(child, SIGSEGV);
	UT_EXPECT_EQ(mapping[0], 1);
	UT_EXPECT_EQ(mapping[page_size * 2], 3);
	UT_ASSERT_EQ(munmap(mapping, page_size), 0);
	UT_ASSERT_EQ(munmap(mapping + page_size * 2, page_size), 0);
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(mapping != MAP_FAILED);
	mapping[0] = 42;
	resized = mremap(mapping, page_size, page_size * 2, MREMAP_MAYMOVE);
	UT_ASSERT(resized != MAP_FAILED);
	UT_EXPECT_EQ(resized[0], 42);
	UT_ASSERT_EQ(munmap(resized, page_size * 2), 0);
}

UT_CASE(mm_file_mapping_msync_mincore_and_madvise, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char resident;
	char *path = ut_path("mapped");
	unsigned char *shared;
	unsigned char *anonymous;
	char check[8] = {};
	int fd;

	UT_ASSERT(page_size > 0);
	UT_ASSERT(path != NULL);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	free(path);
	UT_ASSERT(fd >= 0);
	UT_ASSERT_EQ(ftruncate(fd, (off_t)page_size), 0);
	shared = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	UT_ASSERT(shared != MAP_FAILED);
	memcpy(shared, "mapped", 6);
	UT_ASSERT_EQ(msync(shared, page_size, MS_SYNC), 0);
	UT_ASSERT_EQ(munmap(shared, page_size), 0);
	UT_ASSERT_EQ(pread(fd, check, 6, 0), 6);
	UT_EXPECT_MEMEQ(check, "mapped", 6);
	UT_ASSERT_EQ(close(fd), 0);
	anonymous = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(anonymous != MAP_FAILED);
	anonymous[0] = 91;
	resident = 0;
	UT_ASSERT_EQ(mincore(anonymous, page_size, &resident), 0);
	UT_EXPECT(resident & 1);
	UT_ASSERT_EQ(madvise(anonymous, page_size, MADV_DONTNEED), 0);
	UT_EXPECT_EQ(anonymous[0], 0);
	UT_ASSERT_EQ(munmap(anonymous, page_size), 0);
}

UT_CASE(cow_fork_anon_isolation, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	uint32_t *page;
	pid_t child;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	page[0] = 0x1111;
	child = UT_FORK();
	if (child == 0) {
		/* The child must see the parent's pre-fork value. */
		if (page[0] != 0x1111)
			_exit(90);
		/* The child's write must stay private to the child. */
		page[0] = 0x2222;
		if (page[0] != 0x2222)
			_exit(91);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
	/* The parent's page must be untouched by the child's write. */
	UT_EXPECT_EQ(page[0], 0x1111);
	UT_ASSERT_EQ(munmap(page, page_size), 0);
}

UT_CASE(cow_fork_anonymous_shared_visibility, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *page;
	int ready[2];
	pid_t child;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	UT_ASSERT_EQ(pipe(ready), 0);
	page[0] = 0x11;
	child = UT_FORK();
	if (child == 0) {
		char byte = 'x';

		(void)close(ready[0]);
		page[0] = 0x22;
		if (write(ready[1], &byte, 1) != 1)
			_exit(90);
		(void)close(ready[1]);
		_exit(0);
	}
	UT_ASSERT_EQ(close(ready[1]), 0);
	UT_ASSERT_EQ(read(ready[0], &(char){0}, 1), 1);
	UT_EXPECT_EQ(page[0], 0x22);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(ready[0]), 0);
	UT_ASSERT_EQ(munmap(page, page_size), 0);
}

UT_CASE(cow_fork_file_private, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *mapping;
	unsigned char *disk;
	unsigned char *content;
	char *path;
	pid_t child;
	int fd;

	UT_ASSERT(page_size > 0);
	content = malloc(page_size);
	UT_ASSERT(content != NULL);
	memset(content, 0xaa, page_size);
	UT_ASSERT_EQ(ut_write_file("cow-file", content, page_size, 0600), 0);
	free(content);
	path = ut_path("cow-file");
	UT_ASSERT(path != NULL);
	fd = open(path, O_RDWR);
	free(path);
	UT_ASSERT(fd >= 0);
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE,
		       fd, 0);
	UT_ASSERT(mapping != MAP_FAILED);
	/* A read maps the page-cache page read-only... */
	UT_EXPECT_EQ(mapping[0], 0xaa);
	/* ...and the write COWs it into a private copy. */
	memset(mapping, 0x11, page_size);
	child = UT_FORK();
	if (child == 0) {
		if (mapping[0] != 0x11)
			_exit(90);
		memset(mapping, 0x22, page_size);
		if (mapping[0] != 0x22)
			_exit(91);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
	/* The parent's private copy is untouched by the child. */
	UT_EXPECT_EQ(mapping[0], 0x11);
	UT_ASSERT_EQ(munmap(mapping, page_size), 0);
	/* The on-disk content is unchanged. */
	disk = malloc(page_size);
	UT_ASSERT(disk != NULL);
	UT_ASSERT_EQ(pread(fd, disk, page_size, 0), (ssize_t)page_size);
	UT_EXPECT_EQ(disk[0], 0xaa);
	UT_EXPECT_EQ(disk[page_size - 1], 0xaa);
	free(disk);
	UT_ASSERT_EQ(close(fd), 0);
}

UT_CASE(cow_fork_file_private_cache_page, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *mapping;
	unsigned char *disk;
	unsigned char *content;
	char *path;
	pid_t child;
	int fd;

	UT_ASSERT(page_size > 0);
	content = malloc(page_size);
	UT_ASSERT(content != NULL);
	memset(content, 0xaa, page_size);
	UT_ASSERT_EQ(ut_write_file("cow-cache-file", content, page_size, 0600),
		     0);
	free(content);
	path = ut_path("cow-cache-file");
	UT_ASSERT(path != NULL);
	fd = open(path, O_RDWR);
	free(path);
	UT_ASSERT(fd >= 0);
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE,
		       fd, 0);
	UT_ASSERT(mapping != MAP_FAILED);
	/* Keep this PTE backed by the page cache across fork. */
	UT_ASSERT_EQ(mapping[0], 0xaa);
	child = UT_FORK();
	if (child == 0) {
		if (mapping[0] != 0xaa)
			_exit(90);
		mapping[0] = 0x22;
		if (mapping[0] != 0x22)
			_exit(91);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
	UT_EXPECT_EQ(mapping[0], 0xaa);
	/* The parent must COW from the same cache-backed PTE as well. */
	mapping[0] = 0x11;
	UT_EXPECT_EQ(mapping[0], 0x11);
	UT_ASSERT_EQ(munmap(mapping, page_size), 0);
	disk = malloc(page_size);
	UT_ASSERT(disk != NULL);
	UT_ASSERT_EQ(pread(fd, disk, page_size, 0), (ssize_t)page_size);
	UT_EXPECT_EQ(disk[0], 0xaa);
	UT_EXPECT_EQ(disk[page_size - 1], 0xaa);
	free(disk);
	UT_ASSERT_EQ(close(fd), 0);
}

UT_CASE(cow_write_readonly_never_segfaults, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *mapping;
	pid_t child;

	UT_ASSERT(page_size > 0);
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(mapping != MAP_FAILED);
	mapping[0] = 1;
	UT_ASSERT_EQ(mprotect(mapping, page_size, PROT_READ), 0);
	child = UT_FORK();
	if (child == 0) {
		mapping[0] = 2;
		_exit(127);
	}
	/* A genuinely read-only vma still faults on write. */
	UT_EXPECT_SIGNAL(child, SIGSEGV);
	/* Granting write back must make the write succeed, no signal. */
	UT_ASSERT_EQ(mprotect(mapping, page_size, PROT_READ | PROT_WRITE), 0);
	mapping[0] = 3;
	UT_EXPECT_EQ(mapping[0], 3);
	UT_ASSERT_EQ(munmap(mapping, page_size), 0);
}

UT_CASE(cow_fork_mprotect_isolation, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	uint32_t *page;
	pid_t child;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	page[0] = 0x1111;
	child = UT_FORK();
	if (child == 0) {
		/* mprotect(RW) on a fork-shared page must not grant a
		 * writable view of the shared physical page. */
		if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0)
			_exit(90);
		page[0] = 0x2222;
		if (page[0] != 0x2222)
			_exit(91);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
	/* The parent's page must be untouched by the child's write. */
	UT_EXPECT_EQ(page[0], 0x1111);
	UT_ASSERT_EQ(munmap(page, page_size), 0);
}

UT_CASE(cow_fork_prot_none_and_restore, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *page;
	int child_ready[2];
	int parent_ready[2];
	int child_done[2];
	pid_t child;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	page[0] = 0x11;
	UT_ASSERT_EQ(mprotect(page, page_size, PROT_NONE), 0);
	child = UT_FORK();
	if (child == 0) {
		volatile unsigned char value = page[0];

		(void)value;
		_exit(127);
	}
	UT_EXPECT_SIGNAL(child, SIGSEGV);
	UT_ASSERT_EQ(pipe(child_ready), 0);
	UT_ASSERT_EQ(pipe(parent_ready), 0);
	UT_ASSERT_EQ(pipe(child_done), 0);
	child = UT_FORK();
	if (child == 0) {
		char byte = 'x';

		(void)close(child_ready[0]);
		(void)close(parent_ready[1]);
		(void)close(child_done[0]);
		if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0)
			_exit(90);
		if (page[0] != 0x11)
			_exit(91);
		if (write(child_ready[1], &byte, 1) != 1)
			_exit(92);
		if (read(parent_ready[0], &byte, 1) != 1)
			_exit(93);
		page[0] = 0x22;
		if (page[0] != 0x22)
			_exit(94);
		if (write(child_done[1], &byte, 1) != 1)
			_exit(95);
		_exit(0);
	}
	UT_ASSERT_EQ(close(child_ready[1]), 0);
	UT_ASSERT_EQ(close(parent_ready[0]), 0);
	UT_ASSERT_EQ(close(child_done[1]), 0);
	UT_ASSERT_EQ(read(child_ready[0], &(char){0}, 1), 1);
	UT_ASSERT_EQ(mprotect(page, page_size, PROT_READ | PROT_WRITE), 0);
	UT_EXPECT_EQ(page[0], 0x11);
	page[0] = 0x33;
	UT_ASSERT_EQ(write(parent_ready[1], &(char){'x'}, 1), 1);
	UT_ASSERT_EQ(read(child_done[0], &(char){0}, 1), 1);
	UT_EXPECT_EQ(page[0], 0x33);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(child_ready[0]), 0);
	UT_ASSERT_EQ(close(parent_ready[1]), 0);
	UT_ASSERT_EQ(close(child_done[0]), 0);
	UT_ASSERT_EQ(munmap(page, page_size), 0);
}

UT_CASE(cow_fork_execute_only_stays_unreadable, 5000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *page;
	pid_t child;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	page[0] = 0x11;
	UT_ASSERT_EQ(mprotect(page, page_size, PROT_EXEC), 0);
	child = UT_FORK();
	if (child == 0) {
		volatile unsigned char value = page[0];

		(void)value;
		_exit(127);
	}
	UT_EXPECT_SIGNAL(child, SIGSEGV);
	UT_ASSERT_EQ(munmap(page, page_size), 0);
}

UT_CASE(cow_fork_stress, 10000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	uint32_t *page;
	int i;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	for (i = 0; i < 32; i++) {
		pid_t child;

		page[0] = 0x1111;
		child = UT_FORK();
		if (child == 0) {
			page[0] = 0x2222 + (uint32_t)i;
			_exit(0);
		}
		UT_EXPECT_EXIT(child, 0);
		/* The parent's page is stable across the fork churn. */
		UT_EXPECT_EQ(page[0], 0x1111);
	}
	UT_ASSERT_EQ(munmap(page, page_size), 0);
}

UT_CASE(cow_fork_release_paths, 10000)
{
	const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	uint32_t *page;
	int i;

	UT_ASSERT(page_size > 0);
	page = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UT_ASSERT(page != MAP_FAILED);
	for (i = 0; i < 16; i++) {
		unsigned char *resized;
		pid_t child;

		page[0] = 0x1111;
		page[page_size / sizeof(*page)] = 0x3333;
		child = UT_FORK();
		if (child == 0) {
			resized = mremap(page, page_size * 2, page_size * 3,
					 MREMAP_MAYMOVE);
			if (resized == MAP_FAILED)
				_exit(90);
			resized[0] = 0x22;
			if (madvise(resized + page_size, page_size,
				    MADV_DONTNEED) != 0)
				_exit(91);
			if (munmap(resized, page_size * 3) != 0)
				_exit(92);
			_exit(0);
		}
		UT_EXPECT_EXIT(child, 0);
		UT_EXPECT_EQ(page[0], 0x1111);
		UT_EXPECT_EQ(page[page_size / sizeof(*page)], 0x3333);
	}
	UT_ASSERT_EQ(munmap(page, page_size * 2), 0);
}

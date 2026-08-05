/*
 * Supplementary-groups regression: getgroups/setgroups ABI, credential
 * inheritance across fork, the euid-0 privilege gate, and VFS group-bit
 * consumption via supplementary membership.
 *
 * The runner executes these cases as uid 0; privilege-dropped children use
 * setuid(1000) and encode pass/fail into their exit code (parent reaps with
 * UT_EXPECT_EXIT). Cases stay single-threaded: musl's setgroups wrapper only
 * takes the direct fast path when the process has no other threads.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <utest.h>

#define GROUPS_FILE_GID 2000
#define GROUPS_OTHER_GID 2001
#define GROUPS_UNPRIV_UID 1000

UT_CASE(groups_basic_roundtrip, 1500)
{
	gid_t want[] = {1000, 1001, 1002};
	gid_t got[5];

	/* Fresh root task starts with no supplementary groups. */
	UT_ASSERT_EQ(getgroups(0, NULL), 0);

	/* Positive size with an empty list is legal and copies nothing. */
	UT_ASSERT_EQ(getgroups(2, got), 0);

	UT_ASSERT_EQ(setgroups(3, want), 0);
	UT_ASSERT_EQ(getgroups(0, NULL), 3);
	UT_ASSERT_EQ(getgroups(3, got), 3);
	UT_ASSERT_MEMEQ(got, want, sizeof(want));

	/* An oversized buffer is legal; only ngroups entries are written. */
	UT_ASSERT_EQ(getgroups(5, got), 3);
	UT_ASSERT_MEMEQ(got, want, sizeof(want));

	/* setgroups(0, NULL) clears the list. */
	UT_ASSERT_EQ(setgroups(0, NULL), 0);
	UT_ASSERT_EQ(getgroups(0, NULL), 0);
	/* Linux edge: a positive size with a NULL pointer on an empty list
	 * succeeds (zero-byte copy), do not "fix" this to -EFAULT. */
	UT_ASSERT_EQ(getgroups(1, NULL), 0);

	/* A non-NULL size with a NULL pointer must fault. */
	UT_ASSERT_EQ(setgroups(1, want), 0);
	UT_ASSERT_ERRNO(setgroups(1, NULL), EFAULT);

	/* setgroups must not touch the primary gid. */
	UT_ASSERT_EQ(getgid(), 0);
	UT_ASSERT_EQ(getegid(), 0);

	UT_ASSERT_EQ(setgroups(0, NULL), 0);
}

UT_CASE(groups_errors, 1500)
{
	gid_t want[] = {1000, 1001, 1002};
	gid_t buf[3];

	UT_ASSERT_ERRNO(syscall(SYS_setgroups, -1, NULL), EINVAL);
	UT_ASSERT_ERRNO(syscall(SYS_setgroups, 33, NULL), EINVAL);
	UT_ASSERT_ERRNO(syscall(SYS_getgroups, -1, NULL), EINVAL);

	UT_ASSERT_EQ(setgroups(3, want), 0);
	UT_ASSERT_ERRNO(getgroups(2, buf), EINVAL);
	/* 0xdead0000 lies below TASK_SIZE and is unmapped. */
	UT_ASSERT_ERRNO(getgroups(3, (gid_t *)(uintptr_t)0xdead0000), EFAULT);

	UT_ASSERT_EQ(setgroups(0, NULL), 0);
}

UT_CASE(groups_privilege_drop, 1500)
{
	gid_t want[] = {1000};
	pid_t child;

	child = UT_FORK();
	if (child == 0) {
		if (setuid(GROUPS_UNPRIV_UID) != 0)
			_exit(1);
		errno = 0;
		if (setgroups(0, NULL) != -1 || errno != EPERM)
			_exit(2);
		errno = 0;
		if (setgroups(1, want) != -1 || errno != EPERM)
			_exit(3);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
}

UT_CASE(groups_fork_inheritance, 1500)
{
	gid_t parent_groups[] = {2000, 2001};
	gid_t child_groups[] = {99};
	gid_t got[2];
	pid_t child;

	UT_ASSERT_EQ(setgroups(2, parent_groups), 0);
	child = UT_FORK();
	if (child == 0) {
		/* fork/clone copies the parent cred including the list. */
		if (getgroups(0, NULL) != 2)
			_exit(1);
		if (getgroups(2, got) != 2 ||
		    memcmp(got, parent_groups, sizeof(parent_groups)) != 0)
			_exit(2);
		/* The child mutates its own copy only (COW dup). */
		if (setgroups(1, child_groups) != 0)
			_exit(3);
		if (getgroups(0, NULL) != 1)
			_exit(4);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(getgroups(0, NULL), 2);
	UT_ASSERT_EQ(getgroups(2, got), 2);
	UT_ASSERT_MEMEQ(got, parent_groups, sizeof(parent_groups));

	UT_ASSERT_EQ(setgroups(0, NULL), 0);
}

UT_CASE(groups_vfs_access, 1500)
{
	gid_t member_group = GROUPS_FILE_GID;
	struct stat st;
	pid_t child;

	/* Root-owned files carrying only group bits. */
	UT_ASSERT_NE(open("grpfile", O_RDWR | O_CREAT | O_EXCL, 0600), -1);
	UT_ASSERT_EQ(fchownat(AT_FDCWD, "grpfile", (uid_t)-1,
			      GROUPS_FILE_GID, 0), 0);
	UT_ASSERT_EQ(fchmodat(AT_FDCWD, "grpfile", 0070, 0), 0);
	UT_ASSERT_NE(open("nogrp", O_RDWR | O_CREAT | O_EXCL, 0600), -1);
	UT_ASSERT_EQ(fchownat(AT_FDCWD, "nogrp", (uid_t)-1,
			      GROUPS_OTHER_GID, 0), 0);
	UT_ASSERT_EQ(fchmodat(AT_FDCWD, "nogrp", 0070, 0), 0);
	UT_ASSERT_EQ(stat("grpfile", &st), 0);
	UT_ASSERT_EQ((int)st.st_gid, GROUPS_FILE_GID);
	UT_ASSERT_EQ((int)(st.st_mode & 0777), 0070);

	child = UT_FORK();
	if (child == 0) {
		/* Join the group while still privileged, then drop uid/gid:
		 * egid 1000 != 2000, so only supplementary membership in
		 * {2000} can grant the group bits. */
		if (setgroups(1, &member_group) != 0)
			_exit(1);
		if (setuid(GROUPS_UNPRIV_UID) != 0)
			_exit(2);
		if (setgid(GROUPS_UNPRIV_UID) != 0)
			_exit(3);
		errno = 0;
		if (open("grpfile", O_RDONLY) == -1)
			_exit(4);
		errno = 0;
		if (open("nogrp", O_RDONLY) != -1 || errno != EACCES)
			_exit(5);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
}

UT_CASE(groups_identity_smoke, 1500)
{
	/* Exercises the corrected geteuid/getegid handlers. Without
	 * setreuid/setresuid the four uid fields are always equal, so only
	 * the unchanged root path is observable. */
	UT_ASSERT_EQ(getuid(), 0);
	UT_ASSERT_EQ(geteuid(), 0);
	UT_ASSERT_EQ(getgid(), 0);
	UT_ASSERT_EQ(getegid(), 0);
}

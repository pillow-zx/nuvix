#ifndef _NUVIX_UAPI_STATFS_H
#define _NUVIX_UAPI_STATFS_H

/**
 * @file statfs.h
 * @brief Linux statfs64 UAPI result layout.
 */

/**
 * @struct statfs64
 * @brief Filesystem statistics returned by statfs64/fstatfs64.
 *
 * @par Fields
 * - @c f_type: Filesystem magic/type value.
 * - @c f_bsize: Optimal transfer block size.
 * - @c f_blocks: Total data blocks.
 * - @c f_bfree: Free blocks.
 * - @c f_bavail: Free blocks available to unprivileged users.
 * - @c f_files: Total file nodes.
 * - @c f_ffree: Free file nodes.
 * - @c f_fsid: Filesystem id.
 * - @c f_namelen: Maximum filename length.
 * - @c f_frsize: Fragment size.
 * - @c f_flags: Mount flags.
 * - @c f_spare: Reserved ABI padding.
 */
struct statfs64 {
	long f_type;
	long f_bsize;
	unsigned long f_blocks;
	unsigned long f_bfree;
	unsigned long f_bavail;
	unsigned long f_files;
	unsigned long f_ffree;
	int f_fsid[2];
	long f_namelen;
	long f_frsize;
	long f_flags;
	long f_spare[4];
};

#endif

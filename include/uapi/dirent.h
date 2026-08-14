#ifndef _NUVIX_UAPI_DIRENT_H
#define _NUVIX_UAPI_DIRENT_H

/**
 * @file dirent.h
 * @brief Linux getdents64 directory-entry UAPI layout.
 */

#define DT_UNKNOWN 0
#define DT_FIFO	   1
#define DT_CHR	   2
#define DT_DIR	   4
#define DT_BLK	   6
#define DT_REG	   8
#define DT_LNK	   10
#define DT_SOCK	   12

/**
 * @struct linux_dirent64
 * @brief Variable-length record emitted by getdents64.
 *
 * @par Fields
 * - @c d_ino: Inode number.
 * - @c d_off: Directory stream offset for the next entry.
 * - @c d_reclen: Total record length in bytes.
 * - @c d_type: DT_* file type.
 * - @c d_name: NUL-terminated entry name.
 */
struct linux_dirent64 {
	unsigned long d_ino;
	long d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

#undef offsetof
#define offsetof(t, d) __builtin_offsetof(t, d)

_Static_assert(offsetof(struct linux_dirent64, d_name) == 19,
	       "linux_dirent64 d_name ABI offset mismatch");

#endif

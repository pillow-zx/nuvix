#ifndef _NUVIX_UAPI_STAT_H
#define _NUVIX_UAPI_STAT_H

/**
 * @file stat.h
 * @brief Linux-compatible stat, statx, mode, and dev_t UAPI definitions.
 */

#define S_IFMT	 00170000
#define S_IFSOCK 0140000
#define S_IFLNK	 0120000
#define S_IFREG	 0100000
#define S_IFBLK	 0060000
#define S_IFDIR	 0040000
#define S_IFCHR	 0020000
#define S_IFIFO	 0010000

#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#define S_ISUID 004000
#define S_ISGID 002000
#define S_ISVTX 001000
#define S_IRWXU 00700
#define S_IRWXG 00070
#define S_IRWXO 00007

#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

/**
 * @def MINORBITS
 * @brief Number of low bits reserved for a Linux device minor number.
 */
#define MINORBITS 20u

/**
 * @def MKDEV
 * @brief Pack major/minor numbers into a dev_t-compatible unsigned long.
 */
#define MKDEV(major, minor)                                                    \
	(((unsigned long)(major) << MINORBITS) | (unsigned long)(minor))

/**
 * @def MAJOR
 * @brief Extract the major device number from a packed dev_t value.
 */
#define MAJOR(dev) ((unsigned int)((unsigned long)(dev) >> MINORBITS))

/**
 * @def MINOR
 * @brief Extract the minor device number from a packed dev_t value.
 */
#define MINOR(dev)                                                             \
	((unsigned int)((unsigned long)(dev) & ((1UL << MINORBITS) - 1)))

/**
 * @struct stat
 * @brief Linux riscv64 stat layout returned by fstat/newfstatat.
 *
 * @par Fields
 * - @c st_dev: Device containing the inode.
 * - @c st_ino: Inode number.
 * - @c st_mode: File type and permission bits.
 * - @c st_nlink: Hard-link count.
 * - @c st_uid: Owner uid.
 * - @c st_gid: Owner gid.
 * - @c st_rdev: Device id for special files.
 * - @c __pad1: ABI padding.
 * - @c st_size: File size in bytes.
 * - @c st_blksize: Preferred I/O block size.
 * - @c __pad2: ABI padding.
 * - @c st_blocks: Allocated 512-byte blocks.
 * - @c st_atime_sec: Last access time, seconds.
 * - @c st_atime_nsec: Last access time, nanoseconds.
 * - @c st_mtime_sec: Last data modification time, seconds.
 * - @c st_mtime_nsec: Last modification time, nanoseconds.
 * - @c st_ctime_sec: Last metadata change time, seconds.
 * - @c st_ctime_nsec: Last change time, nanoseconds.
 * - @c st_unused: Reserved ABI padding.
 */
struct stat {
	unsigned long st_dev;
	unsigned long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned long st_rdev;
	unsigned long __pad1;
	long st_size;
	unsigned int st_blksize;
	unsigned int __pad2;
	unsigned long st_blocks;
	long st_atime_sec;
	unsigned long st_atime_nsec;
	long st_mtime_sec;
	unsigned long st_mtime_nsec;
	long st_ctime_sec;
	unsigned long st_ctime_nsec;
	unsigned int st_unused[2];
};

/**
 * @struct statx_timestamp
 * @brief Timestamp layout embedded in Linux statx.
 *
 * @par Fields
 * - @c tv_sec: Whole seconds.
 * - @c tv_nsec: Nanoseconds within the current second.
 * - @c __reserved: Reserved ABI padding.
 */
struct statx_timestamp {
	long tv_sec;
	unsigned int tv_nsec;
	int __reserved;
};

/**
 * @struct statx
 * @brief Linux statx result layout.
 *
 * @par Fields
 * - @c stx_mask: STATX_* bits describing valid fields.
 * - @c stx_blksize: Preferred I/O block size.
 * - @c stx_attributes: Filesystem attribute bits.
 * - @c stx_nlink: Hard-link count.
 * - @c stx_uid: Owner uid.
 * - @c stx_gid: Owner gid.
 * - @c stx_mode: File type and permission bits.
 * - @c __spare0: Reserved ABI padding.
 * - @c stx_ino: Inode number.
 * - @c stx_size: File size in bytes.
 * - @c stx_blocks: Allocated 512-byte blocks.
 * - @c stx_attributes_mask: Supported attribute bits.
 * - @c stx_atime: Last access time.
 * - @c stx_btime: Creation/birth time if known.
 * - @c stx_ctime: Last metadata change time.
 * - @c stx_mtime: Last data modification time.
 * - @c stx_rdev_major: Special-file device major.
 * - @c stx_rdev_minor: Special-file device minor.
 * - @c stx_dev_major: Containing device major.
 * - @c stx_dev_minor: Containing device minor.
 * - @c stx_mnt_id: Mount id when available.
 * - @c stx_dio_mem_align: Direct-I/O memory alignment.
 * - @c stx_dio_offset_align: Direct-I/O offset alignment.
 * - @c stx_subvol: Subvolume id when available.
 * - @c __spare3: Reserved ABI extension space.
 */
struct statx {
	unsigned int stx_mask;
	unsigned int stx_blksize;
	unsigned long stx_attributes;
	unsigned int stx_nlink;
	unsigned int stx_uid;
	unsigned int stx_gid;
	unsigned short stx_mode;
	unsigned short __spare0[1];
	unsigned long stx_ino;
	unsigned long stx_size;
	unsigned long stx_blocks;
	unsigned long stx_attributes_mask;
	struct statx_timestamp stx_atime;
	struct statx_timestamp stx_btime;
	struct statx_timestamp stx_ctime;
	struct statx_timestamp stx_mtime;
	unsigned int stx_rdev_major;
	unsigned int stx_rdev_minor;
	unsigned int stx_dev_major;
	unsigned int stx_dev_minor;
	unsigned long stx_mnt_id;
	unsigned int stx_dio_mem_align;
	unsigned int stx_dio_offset_align;
	unsigned long stx_subvol;
	unsigned long __spare3[11];
};

#define STATX_TYPE	    0x00000001U
#define STATX_MODE	    0x00000002U
#define STATX_NLINK	    0x00000004U
#define STATX_UID	    0x00000008U
#define STATX_GID	    0x00000010U
#define STATX_ATIME	    0x00000020U
#define STATX_MTIME	    0x00000040U
#define STATX_CTIME	    0x00000080U
#define STATX_INO	    0x00000100U
#define STATX_SIZE	    0x00000200U
#define STATX_BLOCKS	    0x00000400U
#define STATX_BASIC_STATS   0x000007ffU
#define STATX_BTIME	    0x00000800U
#define STATX_MNT_ID	    0x00001000U
#define STATX_DIOALIGN	    0x00002000U
#define STATX_MNT_ID_UNIQUE 0x00004000U
#define STATX_SUBVOL	    0x00008000U
#define STATX_ALL	    0x00000fffU
#define STATX__RESERVED	    0x80000000U

#endif

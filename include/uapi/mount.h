#ifndef _NUVIX_UAPI_MOUNT_H
#define _NUVIX_UAPI_MOUNT_H

/**
 * @file mount.h
 * @brief Linux mount(2) and umount2(2) flag constants.
 *
 * These numeric values are UAPI. nuvix currently implements only the
 * documented zero-flag mount model; nonzero flags are exposed so user-space
 * probes can receive stable unsupported errors.
 */

#define MS_RDONLY	1
#define MS_NOSUID	2
#define MS_NODEV	4
#define MS_NOEXEC	8
#define MS_SYNCHRONOUS	16
#define MS_REMOUNT	32
#define MS_MANDLOCK	64
#define MS_DIRSYNC	128
#define MS_NOSYMFOLLOW	256
#define MS_NOATIME	1024
#define MS_NODIRATIME	2048
#define MS_BIND		4096
#define MS_MOVE		8192
#define MS_REC		16384
#define MS_VERBOSE	32768
#define MS_SILENT	32768
#define MS_POSIXACL	(1 << 16)
#define MS_UNBINDABLE	(1 << 17)
#define MS_PRIVATE	(1 << 18)
#define MS_SLAVE	(1 << 19)
#define MS_SHARED	(1 << 20)
#define MS_RELATIME	(1 << 21)
#define MS_KERNMOUNT	(1 << 22)
#define MS_I_VERSION	(1 << 23)
#define MS_STRICTATIME	(1 << 24)
#define MS_LAZYTIME	(1 << 25)
#define MS_SUBMOUNT	(1U << 26)
#define MS_NOREMOTELOCK (1U << 27)
#define MS_NOSEC	(1U << 28)
#define MS_BORN		(1U << 29)
#define MS_ACTIVE	(1U << 30)
#define MS_NOUSER	(1U << 31)

#define MS_RMT_MASK                                                            \
	(MS_RDONLY | MS_SYNCHRONOUS | MS_MANDLOCK | MS_I_VERSION | MS_LAZYTIME)
#define MS_MGC_VAL 0xc0ed0000
#define MS_MGC_MSK 0xffff0000

#define MNT_FORCE	1
#define MNT_DETACH	2
#define MNT_EXPIRE	4
#define UMOUNT_NOFOLLOW 8

#endif

#ifndef _NUVIX_STAT_H
#define _NUVIX_STAT_H

#include <nuvix/tick.h>

#define S_IFMT        00170000
#define S_IFIFO       0010000
#define S_IFCHR       0020000
#define S_IFDIR       0040000
#define S_IFBLK       0060000
#define S_IFREG       0100000
#define S_IFLNK       0120000
#define S_IFSOCK      0140000

#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)

struct kstat {
    uint64_t dev;
    uint64_t ino;
    uint64_t rdev;

    unsigned int mode;
    unsigned int nlink;
    unsigned int uid;
    unsigned int gid;

    loff_t size;
    uint64_t blocks;
    unsigned int blksize;

    struct timespec64 atime;
    struct timespec64 mtime;
    struct timespec64 ctime;
};

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
    int st_blksize;
    int __pad2;
    long st_blocks;
    long st_atime;
    unsigned long st_atime_nsec;
    long st_mtime;
    unsigned long st_mtime_nsec;
    long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned int __unused4;
    unsigned int __unused5;
};

static_assert(sizeof(struct stat) == 128, "invalid stat layout");

#endif /* ifndef _NUVIX_STAT_H */

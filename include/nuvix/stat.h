#ifndef _NUVIX_STAT_H
#define _NUVIX_STAT_H

#include <nuvix/tick.h>

#define S_IFMT        00170000
#define S_IFDIR       0040000
#define S_IFREG       0100000
#define S_IFLNK       0120000

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

#endif /* ifndef _NUVIX_STAT_H */

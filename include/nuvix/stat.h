#ifndef _NUVIX_STAT_H
#define _NUVIX_STAT_H

#include <nuvix/tick.h>

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

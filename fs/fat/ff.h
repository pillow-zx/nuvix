/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem module  R0.16                               /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2025, ChaN, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:

/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/
/----------------------------------------------------------------------------*/

#ifndef FAT32_FF_H
#define FAT32_FF_H

#include <nuvix/ctypes.h>
#include <nuvix/errno.h>

#define FAT_SECTOR_SIZE 512

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef uint32_t LBA_t;
typedef struct FATFS FATFS;

typedef struct {
    FATFS *fs;
    WORD id;
    BYTE attr;
    DWORD sclust;
    FSIZE_t objsize;
} FFOBJID;

typedef struct {
    FFOBJID obj;
    BYTE flag;
    int err;
    FSIZE_t fptr;
    DWORD clust;
    LBA_t dir_sect;
    BYTE *dir_ptr;
} FIL;

typedef struct {
    FFOBJID obj;
    DWORD dptr, clust;
    LBA_t sect;
    BYTE *dir;
    BYTE fn[12];
} DIR;

typedef struct {
    FSIZE_t fsize;
    WORD fdate, ftime;
    BYTE fattrib;
    char fname[13];
} FILINFO;

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS   0x10
#define FA_OPEN_APPEND   0x30

#define AM_RDO           0x01
#define AM_HID           0x02
#define AM_SYS           0x04
#define AM_DIR           0x10
#define AM_ARC           0x20

int f_mount(void);
int f_open(FIL *fp, const char *path, BYTE mode);
int f_close(FIL *fp);
int f_read(FIL *fp, void *buff, UINT btr, UINT *br);
int f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
int f_lseek(FIL *fp, FSIZE_t ofs);
int f_truncate(FIL *fp);
int f_sync(FIL *fp);
int f_opendir(DIR *dp, const char *path);
int f_closedir(DIR *dp);
int f_readdir(DIR *dp, FILINFO *fno);
int f_mkdir(const char *path);
int f_unlink(const char *path);
int f_rename(const char *old_path, const char *new_path);
int f_stat(const char *path, FILINFO *fno);
int f_chdir(const char *path);
int f_getcwd(char *buff, UINT len);
int f_getfree(DWORD *nclst);

#define f_eof(fp)       ((fp)->fptr == (fp)->obj.objsize)
#define f_error(fp)     ((fp)->err)
#define f_tell(fp)      ((fp)->fptr)
#define f_size(fp)      ((fp)->obj.objsize)
#define f_rewind(fp)    f_lseek((fp), 0)
#define f_rewinddir(dp) f_readdir((dp), 0)
#define f_rmdir(path)   f_unlink(path)

#endif

#ifndef FAT32_DISKIO_H
#define FAT32_DISKIO_H

#include "ff.h"

int disk_read(BYTE *buff, LBA_t sector, UINT count);
int disk_write(const BYTE *buff, LBA_t sector, UINT count);
int disk_sync(void);

#endif

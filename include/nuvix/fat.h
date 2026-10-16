#ifndef _NUVIX_FAT_H
#define _NUVIX_FAT_H

#include <nuvix/vfs.h>

int fat_mount(void);

int fat_open(struct file *file, const char *path, unsigned long flags);


#endif /* ifndef _NUVIX_FAT_H */

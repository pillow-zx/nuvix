#include <nuvix/vfs.h>
#include <nuvix/fat.h>

int vfs_mount(void)
{
#ifdef CONFIG_FAT_FS
    return fat_mount();
#endif
}

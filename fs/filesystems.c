/*
 * fs/filesystems.c - built-in filesystem registration
 */

#include <nuvix/vfs.h>

#ifdef CONFIG_EXT2_FS
#include "ext2/ext2.h"
#endif

int filesystems_init(void)
{
	int ret;

#ifdef CONFIG_EXT2_FS
	ret = ext2_init();
	if (ret < 0)
		return ret;
#else
	ret = 0;
#endif

	return ret;
}

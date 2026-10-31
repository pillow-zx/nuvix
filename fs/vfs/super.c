#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/vfs.h>

static struct super_block root_sb;

int vfs_mount(void)
{
#ifdef CONFIG_EXT2_FS
    int ret;

    if (root_sb.s_root)
        return -EBUSY;

    root_sb.s_bdev = root_bdev;
    ret = ext2_mount(&root_sb);
    if (ret) {
        root_sb.s_bdev = NULL;
        return ret;
    }
    return 0;
#else
    return -ENODEV;
#endif
}

int vfs_get_root(struct path *path)
{
    if (!root_sb.s_root)
        return -ENODEV;

    path->dentry = root_sb.s_root;
    path_get(path);
    return 0;
}

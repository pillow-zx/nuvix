#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/syscall.h>
#include <nuvix/uaccess.h>
#include <nuvix/vfs.h>
#include <asm/trap.h>

#define MS_RDONLY  1UL
#define MS_SILENT  (1UL << 15)
#define MS_MGC_VAL 0xc0ed0000UL
#define MS_MGC_MSK 0xffff0000UL

static LIST_HEAD(mounts);
static DEFINE_SPINLOCK(mntlock);
static struct mount *rootmnt;
DEFINE_MUTEX(mount_mutex);

static struct mount *find_mount(const struct path *path)
{
    struct mount *mnt;

    assert_spin_locked(&mntlock);
    list_for_each_entry (mnt, &mounts, node)
        if (mnt->parent == path->mnt && mnt->mntpoint == path->dentry)
            return mnt;
    return NULL;
}

bool is_mntpoint(const struct path *path)
{
    bool found;

    assert_mutex_locked(&mount_mutex);
    spin_lock(&mntlock);
    found = find_mount(path) != NULL;
    spin_unlock(&mntlock);
    return found;
}

void mnt_put(struct mount *mnt)
{
    spin_lock(&mntlock);
    refcount_dec(&mnt->refs);
    spin_unlock(&mntlock);
}

void vfs_follow_mount(struct path *path)
{
    struct mount *mnt;
    struct path next;

    for (;;) {
        spin_lock(&mntlock);
        mnt = find_mount(path);
        if (!mnt) {
            spin_unlock(&mntlock);
            return;
        }
        if (!mnt->unmounting)
            break;
        spin_unlock(&mntlock);
        mutex_lock(&mount_mutex);
        mutex_unlock(&mount_mutex);
    }

    next = (struct path){.mnt = mnt, .dentry = mnt->sb->s_root};
    path_get(&next);
    spin_unlock(&mntlock);
    path_put(path);
    *path = next;
}

static int mnt_get(struct blkdev *dev, const struct file_system_type *type,
                   const struct path *target, unsigned int flags)
{
    struct mount *mnt __free(kfree) = NULL;
    struct mount *entry;

    assert_mutex_locked(&mount_mutex);
    if (dev) {
        list_for_each_entry (entry, &mounts, node)
            if (entry->sb->s_bdev == dev)
                return -EBUSY;
    }

    mnt = kzalloc(sizeof(*mnt));
    if (!mnt)
        return -ENOMEM;
    mnt->sb = vfs_open_super(dev, type, flags);
    if (IS_ERR(mnt->sb))
        return PTR_ERR(mnt->sb);
    if (SB_IS_RDONLY(mnt->sb))
        flags &= ~MNT_WRITE;
    mnt->flags = flags;
    refcount_set(&mnt->refs, 1);
    if (target) {
        path_get(target);
        mnt->parent = target->mnt;
        mnt->mntpoint = target->dentry;
    }

    spin_lock(&mntlock);
    list_add_tail(&mnt->node, &mounts);
    if (!target)
        rootmnt = mnt;
    spin_unlock(&mntlock);
    keep(mnt);
    return 0;
}

int vfs_mount(void)
{
    int ret;

    mutex_lock(&mount_mutex);
    ret = mnt_get(blkdev_first(), NULL, NULL, MNT_READ | MNT_WRITE);
    mutex_unlock(&mount_mutex);
    return ret;
}

int vfs_get_root(struct path *path)
{
    spin_lock(&mntlock);
    if (!rootmnt) {
        spin_unlock(&mntlock);
        return -ENODEV;
    }
    *path = (struct path){.mnt = rootmnt, .dentry = rootmnt->sb->s_root};
    path_get(path);
    spin_unlock(&mntlock);
    return 0;
}

long sys_mount(struct trap_frame *frame)
{
    const char *usource = (const char *)syscallarg(frame, 0);
    const char *utarget = (const char *)syscallarg(frame, 1);
    const char *utype = (const char *)syscallarg(frame, 2);
    unsigned long flags = syscallarg(frame, 3);
    const char *udata = (const char *)syscallarg(frame, 4);
    char *source __free(kfree) = NULL;
    char *target __free(kfree) = NULL;
    char *type __free(kfree) = NULL;
    const struct file_system_type *fstype;
    struct path dst = {0};
    struct inode *inode;
    struct blkdev *dev = NULL;
    unsigned int mntflags = MNT_READ;
    int ret;

    if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
        flags &= ~MS_MGC_MSK;
    if (flags & ~(MS_RDONLY | MS_SILENT))
        return -EINVAL;
    if (udata) {
        char option;

        if (copy_from_user(&option, udata, sizeof(option)))
            return -EFAULT;
        if (option)
            return -EINVAL;
    }
    if (usource) {
        source = get_pathname(usource);
        if (IS_ERR(source))
            return PTR_ERR(move(source));
    }
    target = get_pathname(utarget);
    if (IS_ERR(target))
        return PTR_ERR(move(target));
    type = get_pathname(utype);
    if (IS_ERR(type))
        return PTR_ERR(move(type));
    fstype = vfs_find_type(type);
    if (!fstype)
        return -ENODEV;

    if (!fstype->nodev) {
        struct path src = {0};

        if (!source)
            return -EINVAL;
        ret = vfs_look(source, &src);
        if (ret)
            return ret;
        inode = src.dentry->d_inode;
        dev = S_ISBLK(inode->i_mode) ? blkdev_find(inode->i_rdev) : NULL;
        ret = S_ISBLK(inode->i_mode) ? -ENXIO : -ENOTBLK;
        path_put(&src);
        if (!dev)
            return ret;
    }

    mutex_lock(&mount_mutex);
    ret = vfs_look(target, &dst);
    if (ret)
        goto out;
    inode = dst.dentry->d_inode;
    if (!S_ISDIR(inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }
    mutex_lock(&inode->mutex);
    ret = inode->i_nlink ? 0 : -ENOENT;
    mutex_unlock(&inode->mutex);
    if (ret)
        goto out;
    if (dst.dentry == dst.mnt->sb->s_root || is_mntpoint(&dst)) {
        ret = -EBUSY;
        goto out;
    }
    if (!(flags & MS_RDONLY))
        mntflags |= MNT_WRITE;
    ret = mnt_get(dev, fstype, &dst, mntflags);
out:
    mutex_unlock(&mount_mutex);
    path_put(&dst);
    return ret;
}

long sys_umount2(struct trap_frame *frame)
{
    const char *utarget = (const char *)syscallarg(frame, 0);
    int flags = (int)syscallarg(frame, 1);
    char *target __free(kfree) = NULL;
    struct path dst = {0};
    struct mount *mnt;
    int ret;

    if (flags)
        return -EINVAL;
    target = get_pathname(utarget);
    if (IS_ERR(target))
        return PTR_ERR(move(target));

    mutex_lock(&mount_mutex);
    ret = vfs_look(target, &dst);
    if (ret)
        goto out;
    mnt = dst.mnt;
    if (dst.dentry != mnt->sb->s_root) {
        ret = -EINVAL;
        goto out;
    }
    spin_lock(&mntlock);
    if (!mnt->parent || refcount_read(&mnt->refs) != 2) {
        spin_unlock(&mntlock);
        ret = -EBUSY;
        goto out;
    }
    mnt->unmounting = true;
    spin_unlock(&mntlock);

    path_put(&dst);
    vfs_close_super(mnt->sb, mnt->flags);
    spin_lock(&mntlock);
    list_del(&mnt->node);
    spin_unlock(&mntlock);
    dput(mnt->mntpoint);
    mnt_put(mnt->parent);
    kfree(mnt);
    ret = 0;
out:
    mutex_unlock(&mount_mutex);
    path_put(&dst);
    return ret;
}

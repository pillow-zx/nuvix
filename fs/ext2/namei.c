#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>

#include "ext2.h"

struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode;
    uint32_t ino;
    int ret;

    assert_mutex_locked(&dir->mutex);
    if (!S_ISDIR(dir->i_mode))
        return ERR_PTR(-ENOTDIR);
    if (dentry->d_nlen > VFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);

    ret = ext2_find_ino(dir, dentry, &ino);
    if (ret == -ENOENT)
        return dentry;
    if (ret)
        return ERR_PTR(ret);

    inode = ext2_iget(dir->i_sb, ino);
    if (IS_ERR(inode))
        return ERR_PTR(PTR_ERR(inode));

    dentry->d_inode = inode;
    return dentry;
}

int ext2_create(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode __free(iput) = NULL;
    int ret;

    assert_mutex_locked(&dir->mutex);
    inode = ext2_alloc_inode(dir, S_IFREG);
    if (IS_ERR(inode))
        return PTR_ERR(move(inode));

    mutex_lock(&inode->mutex);
    ret = ext2_add_dirent(dir, dentry->d_name, dentry->d_nlen, inode);
    if (ret)
        inode->i_nlink = 0;
    mutex_unlock(&inode->mutex);
    if (!ret)
        dentry->d_inode = move(inode);
    return ret;
}

struct dentry *ext2_mkdir(struct inode *dir, struct dentry *dentry)
{
    struct cache_page *dp __free(cache_page) = NULL;
    struct cache_page *ip __free(cache_page) = NULL;
    struct inode *inode __free(iput) = NULL;
    uint32_t doff;
    uint32_t ioff;
    int ret;

    assert_mutex_locked(&dir->mutex);
    if (dir->i_nlink >= EXT2_LINK_MAX)
        return ERR_PTR(-EMLINK);
    dp = ext2_get_inode_page(dir, &doff);
    if (IS_ERR(dp))
        return ERR_PTR(PTR_ERR(move(dp)));
    inode = ext2_alloc_inode(dir, S_IFDIR);
    if (IS_ERR(inode))
        return ERR_PTR(PTR_ERR(move(inode)));

    mutex_lock(&inode->mutex);
    ip = ext2_get_inode_page(inode, &ioff);
    if (IS_ERR(ip)) {
        ret = PTR_ERR(move(ip));
        goto out;
    }
    inode->i_nlink = 2;
    ret = ext2_init_dir(inode, dir);
    if (!ret)
        ret = ext2_add_dirent(dir, dentry->d_name, dentry->d_nlen, inode);
    if (!ret) {
        dir->i_nlink++;
        ext2_write_inode(dir, dp, doff);
        ext2_write_inode(inode, ip, ioff);
    }
out:
    if (ret)
        inode->i_nlink = 0;
    mutex_unlock(&inode->mutex);
    if (ret)
        return ERR_PTR(ret);
    dentry->d_inode = move(inode);
    return dentry;
}

int ext2_unlink(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = dentry->d_inode;
    struct cache_page *ip __free(cache_page) = NULL;
    struct cache_page *cp __free(cache_page) = NULL;
    uint32_t ioff;
    uint32_t off;

    assert_mutex_locked(&dir->mutex);
    assert_mutex_locked(&inode->mutex);
    ip = ext2_get_inode_page(inode, &ioff);
    if (IS_ERR(ip))
        return PTR_ERR(move(ip));
    cp = ext2_find_dirent(dir, dentry->d_name, dentry->d_nlen, &off);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    ext2_delete_dirent(dir, cp, off);
    inode->i_nlink--;
    ext2_write_inode(inode, ip, ioff);
    return 0;
}

int ext2_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = dentry->d_inode;
    struct cache_page *dp __free(cache_page) = NULL;
    struct cache_page *ip __free(cache_page) = NULL;
    struct cache_page *cp __free(cache_page) = NULL;
    uint32_t doff;
    uint32_t ioff;
    uint32_t off;
    int ret;

    assert_mutex_locked(&dir->mutex);
    assert_mutex_locked(&inode->mutex);
    ret = ext2_dir_is_empty(inode);
    if (ret <= 0)
        return ret ? ret : -ENOTEMPTY;
    dp = ext2_get_inode_page(dir, &doff);
    if (IS_ERR(dp))
        return PTR_ERR(move(dp));
    ip = ext2_get_inode_page(inode, &ioff);
    if (IS_ERR(ip))
        return PTR_ERR(move(ip));
    cp = ext2_find_dirent(dir, dentry->d_name, dentry->d_nlen, &off);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));

    ext2_delete_dirent(dir, cp, off);
    inode->i_nlink = 0;
    inode->i_size = 0;
    dir->i_nlink--;
    ext2_write_inode(inode, ip, ioff);
    ext2_write_inode(dir, dp, doff);
    return 0;
}

int ext2_symlink(struct inode *dir, struct dentry *dentry, const char *target)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    struct cache_page *ip __free(cache_page) = NULL;
    struct cache_page *cp __free(cache_page) = NULL;
    struct inode *inode __free(iput) = NULL;
    size_t len = strlen(target);
    uint32_t ioff;
    uint32_t blk;
    int ret;

    assert_mutex_locked(&dir->mutex);
    if (len >= sbi->block_size)
        return -ENAMETOOLONG;
    inode = ext2_alloc_inode(dir, S_IFLNK);
    if (IS_ERR(inode))
        return PTR_ERR(move(inode));

    mutex_lock(&inode->mutex);
    ip = ext2_get_inode_page(inode, &ioff);
    if (IS_ERR(ip)) {
        ret = PTR_ERR(move(ip));
        goto out;
    }
    if (len < sizeof(ext2_i(inode)->data)) {
        memcpy(ext2_i(inode)->data, target, len + 1);
    } else {
        ret = ext2_bmap_create(inode, 0, &blk);
        if (ret)
            goto out;
        cp = mapping_get(&sbi->mapping, blk);
        if (IS_ERR(cp)) {
            ret = PTR_ERR(move(cp));
            goto out;
        }
        mutex_lock(&cp->mutex);
        memset(mapping_data(cp), 0, sbi->block_size);
        memcpy(mapping_data(cp), target, len);
        mapping_dirty(cp);
        mutex_unlock(&cp->mutex);
    }
    inode->i_size = len;
    ext2_write_inode(inode, ip, ioff);
    ret = ext2_add_dirent(dir, dentry->d_name, dentry->d_nlen, inode);
out:
    if (ret)
        inode->i_nlink = 0;
    mutex_unlock(&inode->mutex);
    if (!ret)
        dentry->d_inode = move(inode);
    return ret;
}

ssize_t ext2_readlink(struct inode *inode, char *buf, size_t len)
{
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct cache_page *cp __free(cache_page) = NULL;
    size_t limit;
    uint32_t blk;
    ssize_t ret;

    mutex_lock(&inode->mutex);
    Assert(S_ISLNK(inode->i_mode), "readlink requires a symbolic link\n");
    limit = inode->i_blocks ? sbi->block_size : sizeof(ext2_i(inode)->data);
    if ((uint64_t)inode->i_size >= limit) {
        ret = -EUCLEAN;
        goto out;
    }
    len = MIN(len, (size_t)inode->i_size);
    if (!inode->i_blocks) {
        memcpy(buf, ext2_i(inode)->data, len);
    } else {
        ret = ext2_bmap(inode, 0, &blk);
        if (ret)
            goto out;
        if (!blk) {
            ret = -EUCLEAN;
            goto out;
        }
        cp = mapping_get(&sbi->mapping, blk);
        if (IS_ERR(cp)) {
            ret = PTR_ERR(move(cp));
            goto out;
        }
        mutex_lock(&cp->mutex);
        memcpy(buf, mapping_data(cp), len);
        mutex_unlock(&cp->mutex);
    }
    ret = len;
out:
    mutex_unlock(&inode->mutex);
    return ret;
}

int ext2_link(struct dentry *old_dentry, struct inode *dir,
              struct dentry *dentry)
{
    struct inode *inode = old_dentry->d_inode;
    struct cache_page *ip __free(cache_page) = NULL;
    uint32_t off;
    int ret;

    assert_mutex_locked(&dir->mutex);
    assert_mutex_locked(&inode->mutex);
    if (inode->i_nlink >= EXT2_LINK_MAX)
        return -EMLINK;
    ip = ext2_get_inode_page(inode, &off);
    if (IS_ERR(ip))
        return PTR_ERR(move(ip));
    ret = ext2_add_dirent(dir, dentry->d_name, dentry->d_nlen, inode);
    if (ret)
        return ret;

    inode->i_nlink++;
    ext2_write_inode(inode, ip, off);
    ihold(inode);
    dentry->d_inode = inode;
    return 0;
}

int ext2_rename(struct inode *old_dir, struct dentry *old_dentry,
                struct inode *new_dir, struct dentry *new_dentry,
                unsigned int flags)
{
    struct inode *inode = old_dentry->d_inode;
    struct inode *victim = new_dentry->d_inode;
    struct cache_page *old_cp __free(cache_page) = NULL;
    struct cache_page *new_cp __free(cache_page) = NULL;
    struct cache_page *dotdot __free(cache_page) = NULL;
    struct cache_page *old_ip __free(cache_page) = NULL;
    struct cache_page *new_ip __free(cache_page) = NULL;
    struct cache_page *victim_ip __free(cache_page) = NULL;
    bool is_dir = S_ISDIR(inode->i_mode);
    uint32_t old_off;
    uint32_t new_off;
    uint32_t dotdot_off;
    uint32_t old_ioff;
    uint32_t new_ioff;
    uint32_t victim_ioff;
    int ret;

    assert_mutex_locked(&old_dir->mutex);
    assert_mutex_locked(&new_dir->mutex);
    assert_mutex_locked(&inode->mutex);
    if (victim)
        assert_mutex_locked(&victim->mutex);
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && victim)
        return -EEXIST;
    if (inode == victim)
        return 0;

    if (is_dir) {
        if (victim) {
            ret = ext2_dir_is_empty(victim);
            if (ret <= 0)
                return ret ? ret : -ENOTEMPTY;
        } else if (old_dir != new_dir && new_dir->i_nlink >= EXT2_LINK_MAX) {
            return -EMLINK;
        }
        old_ip = ext2_get_inode_page(old_dir, &old_ioff);
        if (IS_ERR(old_ip))
            return PTR_ERR(move(old_ip));
        if (old_dir != new_dir) {
            new_ip = ext2_get_inode_page(new_dir, &new_ioff);
            if (IS_ERR(new_ip))
                return PTR_ERR(move(new_ip));
            dotdot = ext2_find_dirent(inode, "..", 2, &dotdot_off);
            if (IS_ERR(dotdot))
                return PTR_ERR(move(dotdot));
        }
    }
    if (victim) {
        victim_ip = ext2_get_inode_page(victim, &victim_ioff);
        if (IS_ERR(victim_ip))
            return PTR_ERR(move(victim_ip));
        new_cp = ext2_find_dirent(new_dir, new_dentry->d_name,
                                  new_dentry->d_nlen, &new_off);
        if (IS_ERR(new_cp))
            return PTR_ERR(move(new_cp));
    }
    old_cp = ext2_find_dirent(old_dir, old_dentry->d_name, old_dentry->d_nlen,
                              &old_off);
    if (IS_ERR(old_cp))
        return PTR_ERR(move(old_cp));

    if (victim) {
        ext2_set_dirent(new_dir, new_cp, new_off, inode);
        if (is_dir) {
            victim->i_nlink = 0;
            victim->i_size = 0;
        } else {
            victim->i_nlink--;
        }
        ext2_write_inode(victim, victim_ip, victim_ioff);
    } else {
        ret = ext2_add_dirent(new_dir, new_dentry->d_name, new_dentry->d_nlen,
                              inode);
        if (ret)
            return ret;
    }
    ext2_delete_dirent(old_dir, old_cp, old_off);
    if (is_dir) {
        if (old_dir != new_dir) {
            ext2_set_dirent(inode, dotdot, dotdot_off, new_dir);
            if (!victim)
                new_dir->i_nlink++;
            ext2_write_inode(new_dir, new_ip, new_ioff);
        }
        if (old_dir != new_dir || victim)
            old_dir->i_nlink--;
        ext2_write_inode(old_dir, old_ip, old_ioff);
    }
    return 0;
}

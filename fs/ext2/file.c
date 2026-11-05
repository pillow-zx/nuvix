#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/page.h>
#include <nuvix/string.h>

#include "ext2.h"

static int ext2_read_file_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct inode *inode = m->m_host;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    uint64_t off = cp->index * PAGE_SIZE;
    size_t len = MIN((uint64_t)inode->i_size - off, (uint64_t)PAGE_SIZE);
    char *data = mapping_data(cp);
    uint32_t blk;
    uint64_t sect;
    int ret;

    ret = ext2_bmap(inode, (uint32_t)cp->index, &blk);
    if (ret)
        return ret;
    if (!blk) {
        memset(data, 0, PAGE_SIZE);
        return 0;
    }
    if (blk >= sbi->s_es->s_blocks_count)
        return -EIO;

    sect = (uint64_t)blk * PAGE_SIZE / BLK_SECTOR_SIZE;
    ret = blk_read(m->m_bdev, sect, cp->page, 0, PAGE_SIZE);
    if (ret)
        return ret;

    memset(data + len, 0, PAGE_SIZE - len);
    return 0;
}

static int ext2_write_file_page(struct cache_page *cp)
{
    struct mapping *m = cp->mapping;
    struct inode *inode = m->m_host;
    uint32_t blk;
    uint64_t sect;
    int ret;

    ret = ext2_bmap(inode, (uint32_t)cp->index, &blk);
    if (ret)
        return ret;
    Assert(blk, "writeback requires an allocated file block\n");

    sect = (uint64_t)blk * PAGE_SIZE / BLK_SECTOR_SIZE;
    return blk_write(m->m_bdev, sect, cp->page, 0, PAGE_SIZE);
}

static void ext2_mapping_get(struct mapping *m)
{
    ihold(m->m_host);
}

static void ext2_mapping_put(struct mapping *m)
{
    iput(m->m_host);
}

const struct mapping_ops ext2_file_mapping_operations = {
    .get = ext2_mapping_get,
    .put = ext2_mapping_put,
    .read_page = ext2_read_file_page,
    .write_page = ext2_write_file_page,
};

static int ext2_file_open(struct file *file)
{
    (void)file;
    return 0;
}

static ssize_t ext2_read_chunk(struct inode *inode, loff_t pos, void *buf,
                               size_t len)
{
    struct cache_page *cp;
    size_t off = pos % PAGE_SIZE;
    ssize_t ret = 0;

    mutex_lock(&inode->mutex);
    if (pos >= inode->i_size)
        goto out;
    len = MIN(len, (size_t)(inode->i_size - pos));
    cp = mapping_get(&ext2_i(inode)->mapping, (uint64_t)pos / PAGE_SIZE);
    if (IS_ERR(cp)) {
        ret = PTR_ERR(cp);
        goto out;
    }
    mutex_lock(&cp->mutex);
    memcpy(buf, (const char *)mapping_data(cp) + off, len);
    mutex_unlock(&cp->mutex);
    mapping_put(cp);
    ret = len;
out:
    mutex_unlock(&inode->mutex);
    return ret;
}

static ssize_t ext2_file_read(struct kiocb *iocb, struct kiov *iov)
{
    struct inode *inode = iocb->file->f_path.dentry->d_inode;
    struct page *page __free(page) = NULL;
    loff_t pos = iocb->pos;
    size_t len;
    size_t done = 0;
    char *buf;
    int ret = 0;

    if (pos < 0)
        return -EINVAL;
    mutex_lock(&inode->mutex);
    if (pos >= inode->i_size)
        len = 0;
    else
        len = MIN(iov->count, (size_t)inode->i_size - pos);
    mutex_unlock(&inode->mutex);
    if (!len)
        return 0;

    page = alloc_page();
    if (!page)
        return -ENOMEM;
    buf = (char *)__va(page_to_phys(page));
    while (done < len) {
        size_t off = pos % PAGE_SIZE;
        size_t n;
        ssize_t nr;

        nr = ext2_read_chunk(inode, pos, buf,
                             MIN((size_t)PAGE_SIZE - off, len - done));
        if (nr <= 0) {
            ret = nr;
            break;
        }
        n = copy_to_kiov(buf, nr, iov);
        done += n;
        pos += n;
        iocb->pos = pos;
        if (n != (size_t)nr) {
            ret = -EFAULT;
            break;
        }
    }
    return done ? (ssize_t)done : ret;
}

static struct cache_page *ext2_get_write_tail(struct inode *inode, loff_t pos,
                                              uint32_t *blk)
{
    struct cache_page *cp __free(cache_page) = NULL;
    uint64_t idx = (uint64_t)inode->i_size / PAGE_SIZE;
    int ret;

    assert_mutex_locked(&inode->mutex);
    *blk = 0;
    if (pos <= inode->i_size || !(inode->i_size % PAGE_SIZE) ||
        idx == (uint64_t)pos / PAGE_SIZE)
        return NULL;

    cp = mapping_get(&ext2_i(inode)->mapping, idx);
    if (IS_ERR(cp))
        return move(cp);
    ret = ext2_bmap(inode, idx, blk);
    if (ret)
        return ERR_PTR(ret);
    return move(cp);
}

static int ext2_write_chunk(struct inode *inode, loff_t pos, const void *buf,
                            size_t len)
{
    struct cache_page *cp __free(cache_page) = NULL;
    struct cache_page *ip __free(cache_page) = NULL;
    struct cache_page *tail __free(cache_page) = NULL;
    uint64_t idx = (uint64_t)pos / PAGE_SIZE;
    uint64_t end = (uint64_t)pos + len;
    uint64_t old;
    uint32_t off;
    uint32_t blk;
    uint32_t tblk;
    bool dirty;
    int ret;

    mutex_lock(&inode->mutex);
    ip = ext2_get_inode_page(inode, &off);
    if (IS_ERR(ip)) {
        ret = PTR_ERR(move(ip));
        goto out;
    }
    cp = mapping_get(&ext2_i(inode)->mapping, idx);
    if (IS_ERR(cp)) {
        ret = PTR_ERR(move(cp));
        goto out;
    }
    tail = ext2_get_write_tail(inode, pos, &tblk);
    if (IS_ERR(tail)) {
        ret = PTR_ERR(move(tail));
        goto out;
    }
    old = inode->i_blocks;
    ret = ext2_bmap_create(inode, idx, &blk);
    if (ret)
        goto out;
    if (tblk) {
        mutex_lock(&tail->mutex);
        mapping_dirty(tail);
        mutex_unlock(&tail->mutex);
    }
    mutex_lock(&cp->mutex);
    memcpy((char *)mapping_data(cp) + pos % PAGE_SIZE, buf, len);
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
    dirty = inode->i_blocks != old || end > (uint64_t)inode->i_size;
    if (end > (uint64_t)inode->i_size)
        inode->i_size = end;
    if (dirty)
        ext2_write_inode(inode, ip, off);
out:
    mutex_unlock(&inode->mutex);
    return ret;
}

static ssize_t ext2_file_write(struct kiocb *iocb, struct kiov *iov)
{
    struct inode *inode = iocb->file->f_path.dentry->d_inode;
    struct page *page __free(page) = NULL;
    loff_t pos = iocb->pos;
    size_t len = iov->count;
    size_t done = 0;
    char *buf;
    int ret = 0;

    if (pos < 0)
        return -EINVAL;
    if (!len)
        return 0;
    if ((uint64_t)pos > UINT32_MAX || len > UINT32_MAX - (uint64_t)pos)
        return -EFBIG;
    page = alloc_page();
    if (!page)
        return -ENOMEM;
    buf = (char *)__va(page_to_phys(page));
    while (done < len) {
        size_t off = pos % PAGE_SIZE;
        size_t nr = MIN((size_t)PAGE_SIZE - off, len - done);
        size_t n;

        n = copy_from_kiov(buf, nr, iov);
        if (!n) {
            ret = -EFAULT;
            break;
        }
        ret = ext2_write_chunk(inode, pos, buf, n);
        if (ret)
            break;
        done += n;
        pos += n;
        iocb->pos = pos;
        if (n != nr) {
            ret = -EFAULT;
            break;
        }
    }
    return done ? (ssize_t)done : ret;
}

loff_t ext2_llseek(struct file *file, loff_t off, int whence)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    loff_t pos = 0;
    int ret = 0;

    mutex_lock(&file->f_pos_lock);
    mutex_lock(&inode->mutex);
    switch (whence) {
    case SEEK_SET:
        pos = off;
        break;
    case SEEK_CUR:
        if (check_add_overflow(file->f_pos, off, &pos))
            ret = -EOVERFLOW;
        break;
    case SEEK_END:
        if (check_add_overflow(inode->i_size, off, &pos))
            ret = -EOVERFLOW;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    if (!ret && pos < 0)
        ret = -EINVAL;
    if (!ret)
        file->f_pos = pos;
    mutex_unlock(&inode->mutex);
    mutex_unlock(&file->f_pos_lock);

    return ret ? ret : pos;
}

static int ext2_file_fsync(struct file *file)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    int ret;

    mutex_lock(&inode->mutex);
    ret = mapping_flush(&ext2_i(inode)->mapping);
    if (ret)
        goto out;
    ret = mapping_flush(&sbi->mapping);
    if (ret)
        goto out;
    ret = blk_flush(inode->i_sb->s_bdev);
out:
    mutex_unlock(&inode->mutex);
    return ret;
}

static int ext2_file_release(struct file *file)
{
    (void)file;
    return 0;
}

const struct file_operations ext2_file_operations = {
    .open = ext2_file_open,
    .llseek = ext2_llseek,
    .read = ext2_file_read,
    .write = ext2_file_write,
    .fsync = ext2_file_fsync,
    .release = ext2_file_release,
};

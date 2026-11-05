#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/page.h>
#include <nuvix/string.h>

#include "ext2.h"

static const unsigned char ext2_dtype[EXT2_FT_MAX] = {
    [EXT2_FT_UNKNOWN] = DT_UNKNOWN, [EXT2_FT_REG_FILE] = DT_REG,
    [EXT2_FT_DIR] = DT_DIR,         [EXT2_FT_CHRDEV] = DT_CHR,
    [EXT2_FT_BLKDEV] = DT_BLK,      [EXT2_FT_FIFO] = DT_FIFO,
    [EXT2_FT_SOCK] = DT_SOCK,       [EXT2_FT_SYMLINK] = DT_LNK,
};

static uint32_t ext2_dirent_rec_len(uint32_t len)
{
    return ALIGN_UP(sizeof(struct ext2_dirent) + len, 4);
}

static uint8_t ext2_dirent_type(struct inode *inode)
{
    uint8_t type = EXT2_FT_UNKNOWN;

    if (ext2_sb_info(inode->i_sb)->s_es->s_feature_incompat &
        EXT2_FEATURE_INCOMPAT_FILETYPE) {
        switch (inode->i_mode & S_IFMT) {
        case S_IFREG:
            type = EXT2_FT_REG_FILE;
            break;
        case S_IFDIR:
            type = EXT2_FT_DIR;
            break;
        case S_IFCHR:
            type = EXT2_FT_CHRDEV;
            break;
        case S_IFBLK:
            type = EXT2_FT_BLKDEV;
            break;
        case S_IFIFO:
            type = EXT2_FT_FIFO;
            break;
        case S_IFLNK:
            type = EXT2_FT_SYMLINK;
            break;
        case S_IFSOCK:
            type = EXT2_FT_SOCK;
            break;
        }
    }
    return type;
}

static const struct ext2_dirent *ext2_get_dirent(struct ext2_sb_info *sbi,
                                                 const char *buf, uint32_t off,
                                                 uint32_t end)
{
    const struct ext2_dirent *dirent;

    if (end - off < sizeof(*dirent))
        return ERR_PTR(-EUCLEAN);
    dirent = (const struct ext2_dirent *)(buf + off);
    if (dirent->rec_len < sizeof(*dirent) || (dirent->rec_len & 3) ||
        dirent->rec_len > end - off ||
        dirent->name_len > dirent->rec_len - sizeof(*dirent))
        return ERR_PTR(-EUCLEAN);
    if (dirent->inode > sbi->s_es->s_inodes_count)
        return ERR_PTR(-EUCLEAN);
    return dirent;
}

static struct cache_page *ext2_get_dir_page(struct inode *dir, uint32_t lbn)
{
    uint32_t blk;
    int ret;

    ret = ext2_bmap(dir, lbn, &blk);
    if (ret)
        return ERR_PTR(ret);
    if (!blk)
        return NULL;
    return mapping_get(&ext2_sb_info(dir->i_sb)->mapping, blk);
}

static struct cache_page *ext2_append_dir_block(struct inode *dir)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    struct cache_page *ip __free(cache_page) = NULL;
    struct cache_page *cp __free(cache_page) = NULL;
    struct ext2_dirent *dirent;
    uint64_t size = (uint64_t)dir->i_size + sbi->block_size;
    uint32_t off;
    uint32_t blk;
    int ret;

    if (size > UINT32_MAX)
        return ERR_PTR(-EFBIG);
    ip = ext2_get_inode_page(dir, &off);
    if (IS_ERR(ip))
        return move(ip);
    ret = ext2_bmap_create(dir, dir->i_size / sbi->block_size, &blk);
    if (ret)
        return ERR_PTR(ret);
    cp = mapping_get(&sbi->mapping, blk);
    if (IS_ERR(cp)) {
        ext2_write_inode(dir, ip, off);
        return move(cp);
    }
    mutex_lock(&cp->mutex);
    memset(mapping_data(cp), 0, sbi->block_size);
    dirent = mapping_data(cp);
    dirent->rec_len = sbi->block_size;
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
    dir->i_size = size;
    ext2_write_inode(dir, ip, off);
    return move(cp);
}

static int ext2_find_dirent_block(struct ext2_sb_info *sbi, const char *buf,
                                  uint32_t end, const char *name, uint32_t len,
                                  uint32_t *offset)
{
    uint32_t off = 0;

    while (off < end) {
        const struct ext2_dirent *dirent;

        dirent = ext2_get_dirent(sbi, buf, off, end);
        if (IS_ERR(dirent))
            return PTR_ERR(dirent);
        if (dirent->inode && dirent->name_len == len &&
            !memcmp(dirent->name, name, len)) {
            *offset = off;
            return 0;
        }
        off += dirent->rec_len;
    }
    return -ENOENT;
}

struct cache_page *ext2_find_dirent(struct inode *dir, const char *name,
                                    uint32_t len, uint32_t *off)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    uint64_t pos;

    assert_mutex_locked(&dir->mutex);
    for (pos = 0; pos < (uint64_t)dir->i_size; pos += sbi->block_size) {
        struct cache_page *cp __free(cache_page) = NULL;
        uint64_t left = (uint64_t)dir->i_size - pos;
        uint32_t end = left > sbi->block_size ? sbi->block_size : left;
        int ret;

        cp = ext2_get_dir_page(dir, pos / sbi->block_size);
        if (IS_ERR(cp))
            return move(cp);
        if (!cp)
            continue;
        mutex_lock(&cp->mutex);
        ret =
            ext2_find_dirent_block(sbi, mapping_data(cp), end, name, len, off);
        mutex_unlock(&cp->mutex);
        if (!ret)
            return move(cp);
        if (ret != -ENOENT)
            return ERR_PTR(ret);
    }
    return ERR_PTR(-ENOENT);
}

int ext2_find_ino(struct inode *dir, const struct dentry *dentry, uint32_t *ino)
{
    struct cache_page *cp __free(cache_page) = NULL;
    const struct ext2_dirent *dirent;
    uint32_t off;

    cp = ext2_find_dirent(dir, dentry->d_name, dentry->d_nlen, &off);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    mutex_lock(&cp->mutex);
    dirent = (const struct ext2_dirent *)((const char *)mapping_data(cp) + off);
    *ino = dirent->inode;
    mutex_unlock(&cp->mutex);
    return 0;
}

void ext2_set_dirent(struct inode *dir, struct cache_page *cp, uint32_t off,
                     struct inode *inode)
{
    struct ext2_dirent *dirent;

    assert_mutex_locked(&dir->mutex);
    mutex_lock(&cp->mutex);
    dirent = (struct ext2_dirent *)((char *)mapping_data(cp) + off);
    dirent->inode = inode->i_ino;
    dirent->file_type = ext2_dirent_type(inode);
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
}

void ext2_delete_dirent(struct inode *dir, struct cache_page *cp, uint32_t off)
{
    struct ext2_dirent *dirent;
    struct ext2_dirent *prev = NULL;
    char *buf;

    assert_mutex_locked(&dir->mutex);
    mutex_lock(&cp->mutex);
    buf = mapping_data(cp);
    dirent = (struct ext2_dirent *)buf;
    while ((char *)dirent < buf + off) {
        prev = dirent;
        dirent = (struct ext2_dirent *)((char *)dirent + dirent->rec_len);
    }
    if (prev)
        prev->rec_len += dirent->rec_len;
    dirent->inode = 0;
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
}

int ext2_init_dir(struct inode *dir, struct inode *parent)
{
    struct cache_page *cp __free(cache_page) = NULL;
    struct ext2_dirent *dirent;
    uint32_t len = ext2_dirent_rec_len(1);

    assert_mutex_locked(&dir->mutex);
    Assert(!dir->i_size, "directory initialization requires an empty inode\n");
    cp = ext2_append_dir_block(dir);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    mutex_lock(&cp->mutex);
    dirent = mapping_data(cp);
    dirent->inode = dir->i_ino;
    dirent->rec_len = len;
    dirent->name_len = 1;
    dirent->file_type = ext2_dirent_type(dir);
    memcpy(dirent->name, ".", 1);
    dirent = (struct ext2_dirent *)((char *)dirent + len);
    dirent->inode = parent->i_ino;
    dirent->rec_len = ext2_sb_info(dir->i_sb)->block_size - len;
    dirent->name_len = 2;
    dirent->file_type = ext2_dirent_type(parent);
    memcpy(dirent->name, "..", 2);
    mapping_dirty(cp);
    mutex_unlock(&cp->mutex);
    return 0;
}

static int ext2_add_dirent_block(struct inode *dir, struct cache_page *cp,
                                 const char *name, uint32_t len,
                                 struct inode *inode)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    uint32_t need = ext2_dirent_rec_len(len);
    uint32_t off = 0;
    char *buf = mapping_data(cp);

    while (off < sbi->block_size) {
        const struct ext2_dirent *dirent;
        struct ext2_dirent *slot;
        uint32_t used;
        uint32_t rec_len;

        dirent = ext2_get_dirent(sbi, buf, off, sbi->block_size);
        if (IS_ERR(dirent))
            return PTR_ERR(dirent);
        rec_len = dirent->rec_len;
        used = dirent->inode ? ext2_dirent_rec_len(dirent->name_len) : 0;
        if (rec_len - used >= need) {
            slot = (struct ext2_dirent *)(buf + off);
            if (used) {
                slot->rec_len = used;
                slot = (struct ext2_dirent *)((char *)slot + used);
            }
            slot->inode = inode->i_ino;
            slot->rec_len = rec_len - used;
            slot->name_len = len;
            slot->file_type = ext2_dirent_type(inode);
            memcpy(slot->name, name, len);
            mapping_dirty(cp);
            return 0;
        }
        off += rec_len;
    }
    return -ENOSPC;
}

int ext2_add_dirent(struct inode *dir, const char *name, uint32_t len,
                    struct inode *inode)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    struct cache_page *cp __free(cache_page) = NULL;
    uint64_t pos;
    int ret;

    assert_mutex_locked(&dir->mutex);
    if (dir->i_size % sbi->block_size)
        return -EUCLEAN;
    for (pos = 0; pos < (uint64_t)dir->i_size; pos += sbi->block_size) {
        struct cache_page *page __free(cache_page) = NULL;

        page = ext2_get_dir_page(dir, pos / sbi->block_size);
        if (IS_ERR(page))
            return PTR_ERR(move(page));
        if (!page)
            continue;
        mutex_lock(&page->mutex);
        ret = ext2_add_dirent_block(dir, page, name, len, inode);
        mutex_unlock(&page->mutex);
        if (ret != -ENOSPC)
            return ret;
    }
    cp = ext2_append_dir_block(dir);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    mutex_lock(&cp->mutex);
    ret = ext2_add_dirent_block(dir, cp, name, len, inode);
    mutex_unlock(&cp->mutex);
    return ret;
}

static int ext2_dir_is_empty_block(struct inode *dir, const char *buf,
                                   uint32_t end)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    uint32_t off = 0;

    while (off < end) {
        const struct ext2_dirent *dirent;

        dirent = ext2_get_dirent(sbi, buf, off, end);
        if (IS_ERR(dirent))
            return PTR_ERR(dirent);
        off += dirent->rec_len;
        if (!dirent->inode)
            continue;
        if (dirent->name_len == 1 && dirent->name[0] == '.' &&
            dirent->inode == dir->i_ino)
            continue;
        if (dirent->name_len == 2 && !memcmp(dirent->name, "..", 2))
            continue;
        return 0;
    }
    return 1;
}

int ext2_dir_is_empty(struct inode *dir)
{
    struct ext2_sb_info *sbi = ext2_sb_info(dir->i_sb);
    uint64_t pos;

    assert_mutex_locked(&dir->mutex);
    for (pos = 0; pos < (uint64_t)dir->i_size; pos += sbi->block_size) {
        struct cache_page *cp __free(cache_page) = NULL;
        uint64_t left = (uint64_t)dir->i_size - pos;
        uint32_t end = left > sbi->block_size ? sbi->block_size : left;
        int ret;

        cp = ext2_get_dir_page(dir, pos / sbi->block_size);
        if (IS_ERR(cp))
            return PTR_ERR(move(cp));
        if (!cp)
            continue;
        mutex_lock(&cp->mutex);
        ret = ext2_dir_is_empty_block(dir, mapping_data(cp), end);
        mutex_unlock(&cp->mutex);
        if (ret != 1)
            return ret;
    }
    return 1;
}

static ssize_t ext2_dir_read(struct kiocb *iocb, struct kiov *iov)
{
    (void)iocb;
    (void)iov;
    return -EISDIR;
}

static int ext2_read_dir_block(struct inode *inode, uint32_t lbn, char *buf,
                               uint32_t end)
{
    struct cache_page *cp __free(cache_page) = NULL;

    cp = ext2_get_dir_page(inode, lbn);
    if (IS_ERR(cp))
        return PTR_ERR(move(cp));
    if (!cp)
        return 0;
    mutex_lock(&cp->mutex);
    memcpy(buf, mapping_data(cp), end);
    mutex_unlock(&cp->mutex);
    return 1;
}

static int ext2_readdir_block(struct ext2_sb_info *sbi, const char *buf,
                              uint32_t end, struct dirctx *ctx)
{
    uint32_t off = ctx->pos % sbi->block_size;

    while (off < end) {
        const struct ext2_dirent *dirent;
        loff_t next;

        dirent = ext2_get_dirent(sbi, buf, off, end);
        if (IS_ERR(dirent))
            return PTR_ERR(dirent);
        next = ctx->pos + dirent->rec_len;
        if (dirent->inode) {
            unsigned char type = DT_UNKNOWN;

            if ((sbi->s_es->s_feature_incompat &
                 EXT2_FEATURE_INCOMPAT_FILETYPE) &&
                dirent->file_type < EXT2_FT_MAX)
                type = ext2_dtype[dirent->file_type];
            if (!dir_emit(ctx, dirent->name, dirent->name_len, next,
                          dirent->inode, type))
                return 0;
        }
        ctx->pos = next;
        off += dirent->rec_len;
    }
    return 1;
}

static int ext2_dir_readdir(struct file *file, struct dirctx *ctx)
{
    struct inode *inode = file->f_path.dentry->d_inode;
    struct ext2_sb_info *sbi = ext2_sb_info(inode->i_sb);
    struct page *page __free(page) = NULL;
    char *buf;

    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;
    page = alloc_page();
    if (!page)
        return -ENOMEM;
    buf = (char *)__va(page_to_phys(page));

    for (;;) {
        uint32_t lbn;
        uint32_t off;
        uint64_t left;
        uint32_t end;
        int ret;

        mutex_lock(&inode->mutex);
        if (ctx->pos >= inode->i_size) {
            mutex_unlock(&inode->mutex);
            return 0;
        }
        lbn = ctx->pos / sbi->block_size;
        off = ctx->pos % sbi->block_size;
        left = inode->i_size - ctx->pos;
        end = off + left > sbi->block_size ? sbi->block_size : off + left;
        ret = ext2_read_dir_block(inode, lbn, buf, end);
        if (!ret)
            ctx->pos = ((loff_t)lbn + 1) * sbi->block_size;
        mutex_unlock(&inode->mutex);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        ret = ext2_readdir_block(sbi, buf, end, ctx);
        if (ret <= 0)
            return ret;
    }
}

const struct file_operations ext2_dir_ops = {
    .llseek = ext2_llseek,
    .read = ext2_dir_read,
    .readdir = ext2_dir_readdir,
};

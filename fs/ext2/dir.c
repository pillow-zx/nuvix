#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/slab.h>
#include <nuvix/vfs.h>

#include "ext2.h"

#define EXT2_DIR_REC_LEN(name_len) (((name_len) + 8 + 3) & ~3u)

static int ext2_sync_dir_page(struct pgcache *page)
{
	return pgcache_sync_page(page) < 0 ? -EIO : 0;
}

static struct pgcache *
ext2_read_inode_page(struct inode *inode, uint32_t lblock)
{
	return inode ? pgcache_get_mapping(&inode->i_pages, lblock,
					      PAGE_CACHE_READ, NULL)
		     : NULL;
}

static uint8_t ext2_file_type(uint16_t mode)
{
	switch (mode & EXT2_S_IFMT) {
	case EXT2_S_IFDIR:
		return EXT2_FT_DIR;
	case EXT2_S_IFCHR:
		return EXT2_FT_CHRDEV;
	case EXT2_S_IFBLK:
		return EXT2_FT_BLKDEV;
	case EXT2_S_IFIFO:
		return EXT2_FT_FIFO;
	case EXT2_S_IFLNK:
		return EXT2_FT_SYMLINK;
	case EXT2_S_IFSOCK:
		return EXT2_FT_SOCK;
	case EXT2_S_IFREG:
	default:
		return EXT2_FT_REG_FILE;
	}
}

static bool ext2_match(struct ext2_dir_entry_2 *de, const char *name,
		       size_t namelen)
{
	if (!de->inode || de->name_len != namelen)
		return false;
	return memcmp(de->name, name, namelen) == 0;
}
static void ext2_dirent_init(struct ext2_dir_entry_2 *de, uint32_t ino,
			     uint16_t rec_len, const char *name, size_t namelen,
			     uint8_t type)
{
	de->inode = ino;
	de->rec_len = rec_len;
	de->name_len = (uint8_t)namelen;
	de->file_type = type;
	memcpy(de->name, name, namelen);
}

static struct pgcache *ext2_new_inode_page(struct inode *inode,
					      uint32_t lblock)
{
	struct pgcache *page;

	page = pgcache_get_mapping(&inode->i_pages, lblock,
				      PAGE_CACHE_CREATE, NULL);
	if (!page)
		return NULL;

	if (!pgcache_is_uptodate(page)) {
		memset(page_cache_data(page), 0, BLOCK_SIZE);
		pgcache_set_uptodate(page, true);
	}

	return page;
}

static struct ext2_dir_entry_2 *ext2_find_entry(struct inode *dir,
						const char *name,
						size_t namelen,
						struct pgcache **res_page)
{
	uint32_t blocks =
		(uint32_t)((dir->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE);

	for (uint32_t lblock = 0; lblock < blocks; lblock++) {
		struct pgcache *page;
		uint8_t *data;
		uint32_t offset = 0;

		if (!ext2_bmap_readonly(dir, lblock))
			continue;
		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			continue;
		data = page_cache_data(page);

		while (offset + 8 <= BLOCK_SIZE) {
			struct ext2_dir_entry_2 *de =
				(struct ext2_dir_entry_2 *)(data + offset);

			if (de->rec_len < 8 ||
			    offset + de->rec_len > BLOCK_SIZE)
				break;
			if (ext2_match(de, name, namelen)) {
				*res_page = page;
				return de;
			}
			offset += de->rec_len;
		}
		pgcache_put_page(page);
	}

	return NULL;
}

static int ext2_add_entry(struct inode *dir, const char *name, size_t namelen,
			  uint32_t ino, uint8_t type)
{
	struct pgcache *found_page = NULL;
	uint16_t need = EXT2_DIR_REC_LEN(namelen);
	uint32_t blocks =
		(uint32_t)((dir->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE);
	uint32_t new_block;
	int ret;

	if (ext2_find_entry(dir, name, namelen, &found_page)) {
		pgcache_put_page(found_page);
		return -EEXIST;
	}

	for (uint32_t lblock = 0; lblock < blocks; lblock++) {
		struct pgcache *page;
		uint8_t *data;
		uint32_t offset = 0;

		if (!ext2_bmap_readonly(dir, lblock))
			continue;
		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			return -EIO;
		data = page_cache_data(page);

		while (offset + 8 <= BLOCK_SIZE) {
			struct ext2_dir_entry_2 *de =
				(struct ext2_dir_entry_2 *)(data + offset);
			uint16_t used;
			uint16_t spare;

			if (de->rec_len < 8 ||
			    offset + de->rec_len > BLOCK_SIZE)
				break;

			if (!de->inode && de->rec_len >= need) {
				ext2_dirent_init(de, ino, de->rec_len, name,
						 namelen, type);

				pgcache_mark_dirty(page);
				if (ext2_sync_dir_page(page) < 0) {
					pgcache_put_page(page);
					return -EIO;
				}
				pgcache_put_page(page);
				return 0;
			}

			used = EXT2_DIR_REC_LEN(de->name_len);
			spare = de->rec_len - used;
			if (spare >= need) {
				struct ext2_dir_entry_2 *new_de;

				de->rec_len = used;
				new_de =
					(struct ext2_dir_entry_2 *)((uint8_t *)
									    de +
								    used);
				ext2_dirent_init(new_de, ino, spare, name,
						 namelen, type);
				pgcache_mark_dirty(page);
				if (ext2_sync_dir_page(page) < 0) {
					pgcache_put_page(page);
					return -EIO;
				}
				pgcache_put_page(page);
				return 0;
			}

			offset += de->rec_len;
		}
		pgcache_put_page(page);
	}

	ret = ext2_bmap(dir, blocks, true, &new_block);
	if (ret < 0)
		return ret;
	if (!new_block)
		return -ENOSPC;

	struct pgcache *page = ext2_new_inode_page(dir, blocks);
	if (!page)
		return -EIO;

	uint8_t *data = page_cache_data(page);
	memset(data, 0, BLOCK_SIZE);
	struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)data;
	ext2_dirent_init(de, ino, BLOCK_SIZE, name, namelen, type);
	pgcache_mark_dirty(page);
	if (ext2_sync_dir_page(page) < 0) {
		pgcache_put_page(page);
		return -EIO;
	}
	pgcache_put_page(page);

	dir->i_size += BLOCK_SIZE;
	return ext2_write_inode(dir);
}

static int ext2_delete_entry(struct inode *dir, struct dentry *dentry)
{
	uint32_t blocks =
		(uint32_t)((dir->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE);

	for (uint32_t lblock = 0; lblock < blocks; lblock++) {
		struct pgcache *page;
		struct ext2_dir_entry_2 *prev = NULL;
		uint8_t *data;
		uint32_t offset = 0;

		if (!ext2_bmap_readonly(dir, lblock))
			continue;
		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			return -EIO;
		data = page_cache_data(page);

		while (offset + 8 <= BLOCK_SIZE) {
			struct ext2_dir_entry_2 *de =
				(struct ext2_dir_entry_2 *)(data + offset);

			if (de->rec_len < 8 ||
			    offset + de->rec_len > BLOCK_SIZE)
				break;
			if (ext2_match(de, dentry->d_name, dentry->d_namelen)) {
				if (prev)
					prev->rec_len += de->rec_len;
				else
					de->inode = 0;
				pgcache_mark_dirty(page);
				if (ext2_sync_dir_page(page) < 0) {
					pgcache_put_page(page);
					return -EIO;
				}
				pgcache_put_page(page);
				return 0;
			}
			prev = de;
			offset += de->rec_len;
		}
		pgcache_put_page(page);
	}

	return -ENOENT;
}

static int ext2_replace_entry(struct inode *dir, struct dentry *dentry,
			      uint32_t ino, uint8_t type)
{
	struct pgcache *page = NULL;
	struct ext2_dir_entry_2 *de;

	de = ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page);
	if (!de)
		return -ENOENT;

	de->inode = ino;
	de->file_type = type;
	pgcache_mark_dirty(page);
	if (ext2_sync_dir_page(page) < 0) {
		pgcache_put_page(page);
		return -EIO;
	}
	pgcache_put_page(page);
	return 0;
}

static void ext2_rollback_new_inode(struct inode *inode)
{
	if (!inode)
		return;

	inode->i_nlink = 0;
	inode_forget(inode);
}

static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
	struct pgcache *page = NULL;
	struct ext2_dir_entry_2 *de;
	struct inode *inode;

	if (!dir || !dentry)
		return NULL;
	if ((dir->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
		return NULL;

	de = ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page);
	if (!de)
		return NULL;

	inode = iget(dir->i_sb, de->inode);
	pgcache_put_page(page);
	if (!inode)
		return NULL;

	dentry->d_inode = inode;
	dentry->d_sb = dir->i_sb;
	return dentry;
}

static int ext2_create(struct inode *dir, struct dentry *dentry, uint32_t mode)
{
	struct pgcache *page = NULL;
	uint32_t ino;
	uint32_t type = mode & EXT2_S_IFMT;
	uint32_t inode_mode;
	struct inode *inode;
	struct ext2_inode_info *ei;
	int ret;

	if (ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page)) {
		pgcache_put_page(page);
		return -EEXIST;
	}

	if (type == 0)
		type = EXT2_S_IFREG;
	inode_mode = type | (mode & ~EXT2_S_IFMT);

	ino = ext2_alloc_inode(dir->i_sb, (uint16_t)inode_mode);
	if (!ino)
		return -ENOSPC;

	inode = iget(dir->i_sb, ino);
	if (!inode) {
		ext2_free_inode(dir->i_sb, ino);
		return -EIO;
	}

	ei = EXT2_I(inode);
	memset(&ei->raw_inode, 0, sizeof(ei->raw_inode));
	inode->i_mode = inode_mode;
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode->i_rdev = 0;
	ext2_init_inode_ops(inode);
	ret = ext2_write_inode(inode);
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}

	ret = ext2_add_entry(dir, dentry->d_name, dentry->d_namelen, ino,
			     ext2_file_type((uint16_t)inode->i_mode));
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}

	dentry->d_inode = inode;
	dentry->d_sb = dir->i_sb;
	return 0;
}

static int ext2_symlink(struct inode *dir, struct dentry *dentry,
			const char *target)
{
	struct pgcache *page = NULL;
	size_t len = strlen(target);
	uint32_t ino;
	struct inode *inode;
	struct ext2_inode_info *ei;
	int ret;

	if (len == 0)
		return -ENOENT;
	if (len > BLOCK_SIZE)
		return -ENAMETOOLONG;
	if (ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page)) {
		pgcache_put_page(page);
		return -EEXIST;
	}

	ino = ext2_alloc_inode(dir->i_sb, EXT2_S_IFLNK | 0777);
	if (!ino)
		return -ENOSPC;

	inode = iget(dir->i_sb, ino);
	if (!inode) {
		ext2_free_inode(dir->i_sb, ino);
		return -EIO;
	}

	ei = EXT2_I(inode);
	memset(&ei->raw_inode, 0, sizeof(ei->raw_inode));
	inode->i_mode = EXT2_S_IFLNK | 0777;
	inode->i_nlink = 1;
	inode->i_size = len;
	inode->i_blocks = 0;
	inode->i_rdev = 0;
	ext2_init_inode_ops(inode);

	if (len <= sizeof(ei->raw_inode.i_block)) {
		memcpy(ei->raw_inode.i_block, target, len);
	} else {
		uint32_t block;
		struct pgcache *target_page;

		ret = ext2_bmap(inode, 0, true, &block);
		if (ret < 0) {
			ext2_rollback_new_inode(inode);
			return ret;
		}
		if (!block) {
			ext2_rollback_new_inode(inode);
			return -ENOSPC;
		}
		target_page = ext2_new_inode_page(inode, 0);
		if (!target_page) {
			ext2_rollback_new_inode(inode);
			return -EIO;
		}
		memset(page_cache_data(target_page), 0, BLOCK_SIZE);
		memcpy(page_cache_data(target_page), target, len);
		pgcache_mark_dirty(target_page);
		if (ext2_sync_dir_page(target_page) < 0) {
			pgcache_put_page(target_page);
			ext2_rollback_new_inode(inode);
			return -EIO;
		}
		pgcache_put_page(target_page);
	}
	ret = ext2_write_inode(inode);
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}

	ret = ext2_add_entry(dir, dentry->d_name, dentry->d_namelen, ino,
			     EXT2_FT_SYMLINK);
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}
	ret = vfs_inode_touch(dir, false, true, true);
	if (ret < 0) {
		ext2_delete_entry(dir, dentry);
		ext2_rollback_new_inode(inode);
		return ret;
	}

	dentry->d_inode = inode;
	dentry->d_sb = dir->i_sb;
	return 0;
}

static int ext2_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *new_dentry)
{
	struct inode *inode;
	int err;
	int ret;

	if (!old_dentry || !old_dentry->d_inode || !new_dentry)
		return -ENOENT;

	inode = old_dentry->d_inode;
	if ((inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
		return -EPERM;
	if (inode->i_sb != dir->i_sb)
		return -EXDEV;

	ret = ext2_add_entry(dir, new_dentry->d_name, new_dentry->d_namelen,
			     (uint32_t)inode->i_ino,
			     ext2_file_type((uint16_t)inode->i_mode));
	if (ret < 0)
		return ret;

	inode->i_nlink++;
	ret = vfs_inode_touch(inode, false, false, true);
	if (ret < 0) {
		err = ret;
		ext2_delete_entry(dir, new_dentry);
		inode->i_nlink--;
		ret = ext2_write_inode(inode);
		if (ret < 0)
			return ret;
		return err;
	}
	ret = vfs_inode_touch(dir, false, true, true);
	if (ret < 0) {
		err = ret;
		ext2_delete_entry(dir, new_dentry);
		inode->i_nlink--;
		ret = ext2_write_inode(inode);
		if (ret < 0)
			return ret;
		return err;
	}

	igrab(inode);
	new_dentry->d_inode = inode;
	new_dentry->d_sb = dir->i_sb;
	return 0;
}

static int ext2_make_empty_dir(struct inode *inode, struct inode *parent)
{
	uint32_t block;
	struct pgcache *page;
	struct ext2_dir_entry_2 *de;
	uint8_t *data;
	int ret;

	ret = ext2_bmap(inode, 0, true, &block);
	if (ret < 0)
		return ret;
	if (!block)
		return -ENOSPC;

	page = ext2_new_inode_page(inode, 0);
	if (!page)
		return -EIO;
	data = page_cache_data(page);

	memset(data, 0, BLOCK_SIZE);
	de = (struct ext2_dir_entry_2 *)data;
	ext2_dirent_init(de, (uint32_t)inode->i_ino, EXT2_DIR_REC_LEN(1), ".",
			 1, EXT2_FT_DIR);

	de = (struct ext2_dir_entry_2 *)(data + de->rec_len);
	ext2_dirent_init(de, (uint32_t)parent->i_ino,
			 BLOCK_SIZE - EXT2_DIR_REC_LEN(1), "..", 2,
			 EXT2_FT_DIR);

	pgcache_mark_dirty(page);
	if (ext2_sync_dir_page(page) < 0) {
		pgcache_put_page(page);
		return -EIO;
	}
	pgcache_put_page(page);
	inode->i_size = BLOCK_SIZE;
	return ext2_write_inode(inode);
}

static int ext2_mkdir(struct inode *dir, struct dentry *dentry, uint32_t mode)
{
	struct pgcache *page = NULL;
	uint32_t ino;
	struct inode *inode;
	struct ext2_inode_info *ei;
	int ret;

	if (ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page)) {
		pgcache_put_page(page);
		return -EEXIST;
	}

	ino = ext2_alloc_inode(dir->i_sb, (uint16_t)(EXT2_S_IFDIR | mode));
	if (!ino)
		return -ENOSPC;

	inode = iget(dir->i_sb, ino);
	if (!inode) {
		ext2_free_inode(dir->i_sb, ino);
		return -EIO;
	}

	ei = EXT2_I(inode);
	memset(&ei->raw_inode, 0, sizeof(ei->raw_inode));
	inode->i_mode = EXT2_S_IFDIR | mode;
	inode->i_nlink = 2;
	inode->i_size = 0;
	inode->i_blocks = 0;
	ext2_init_inode_ops(inode);
	ret = ext2_write_inode(inode);
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}

	ret = ext2_make_empty_dir(inode, dir);
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}

	ret = ext2_add_entry(dir, dentry->d_name, dentry->d_namelen, ino,
			     ext2_file_type((uint16_t)inode->i_mode));
	if (ret < 0) {
		ext2_rollback_new_inode(inode);
		return ret;
	}

	dir->i_nlink++;
	ret = ext2_write_inode(dir);
	if (ret < 0)
		return ret;
	dentry->d_inode = inode;
	dentry->d_sb = dir->i_sb;
	return 0;
}

static bool ext2_dir_is_empty(struct inode *inode)
{
	uint32_t blocks =
		(uint32_t)((inode->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE);

	for (uint32_t lblock = 0; lblock < blocks; lblock++) {
		struct pgcache *page;
		uint8_t *data;
		uint32_t offset = 0;

		if (!ext2_bmap_readonly(inode, lblock))
			continue;
		page = ext2_read_inode_page(inode, lblock);
		if (!page)
			return false;
		data = page_cache_data(page);

		while (offset + 8 <= BLOCK_SIZE) {
			struct ext2_dir_entry_2 *de =
				(struct ext2_dir_entry_2 *)(data + offset);
			bool dot;

			if (de->rec_len < 8 ||
			    offset + de->rec_len > BLOCK_SIZE)
				break;
			dot = (de->name_len == 1 && de->name[0] == '.') ||
			      (de->name_len == 2 && de->name[0] == '.' &&
			       de->name[1] == '.');
			if (de->inode && !dot) {
				pgcache_put_page(page);
				return false;
			}
			offset += de->rec_len;
		}
		pgcache_put_page(page);
	}

	return true;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	int ret;

	if (!inode)
		return -ENOENT;
	if ((inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
		return -EISDIR;

	ret = ext2_delete_entry(dir, dentry);
	if (ret < 0)
		return ret;

	if (inode->i_nlink > 0)
		inode->i_nlink--;
	ret = ext2_write_inode(inode);
	dentry->d_inode = NULL;
	iput(inode);
	return ret;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	int ret;

	if (!inode)
		return -ENOENT;
	if ((inode->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
		return -ENOTDIR;
	if (!ext2_dir_is_empty(inode))
		return -ENOTEMPTY;

	ret = ext2_delete_entry(dir, dentry);
	if (ret < 0)
		return ret;

	if (dir->i_nlink > 0)
		dir->i_nlink--;
	inode->i_nlink = 0;
	ret = ext2_write_inode(inode);
	if (ret == 0)
		ret = ext2_write_inode(dir);
	dentry->d_inode = NULL;
	iput(inode);
	return ret;
}

static int ext2_readdir(struct file *file, void *ctx, filldir_t filldir)
{
	struct inode *dir = file->f_inode;

	while ((uint64_t)file->f_pos < dir->i_size) {
		uint32_t lblock =
			(uint32_t)((uint64_t)file->f_pos / BLOCK_SIZE);
		uint32_t offset =
			(uint32_t)((uint64_t)file->f_pos % BLOCK_SIZE);
		struct pgcache *page;
		struct ext2_dir_entry_2 *de;
		uint8_t *data;

		if (!ext2_bmap_readonly(dir, lblock)) {
			file->f_pos = (loff_t)((lblock + 1) * BLOCK_SIZE);
			continue;
		}

		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			return -EIO;
		data = page_cache_data(page);

		de = (struct ext2_dir_entry_2 *)(data + offset);
		if (offset + 8 > BLOCK_SIZE || de->rec_len < 8 ||
		    offset + de->rec_len > BLOCK_SIZE) {
			pgcache_put_page(page);
			return -EIO;
		}

		loff_t next_pos = file->f_pos + de->rec_len;
		if (de->inode) {
			int ret = filldir(ctx, de->name, de->name_len,
					  de->inode, de->file_type, next_pos);

			if (ret < 0) {
				pgcache_put_page(page);
				return ret;
			}
		}

		file->f_pos = next_pos;
		pgcache_put_page(page);
	}

	return 0;
}

static int ext2_set_dotdot(struct inode *dir, uint32_t new_parent_ino)
{
	uint32_t blocks =
		(uint32_t)((dir->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE);

	for (uint32_t lblock = 0; lblock < blocks; lblock++) {
		struct pgcache *page;
		uint8_t *data;
		uint32_t offset = 0;

		if (!ext2_bmap_readonly(dir, lblock))
			continue;
		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			return -EIO;
		data = page_cache_data(page);

		while (offset + 8 <= BLOCK_SIZE) {
			struct ext2_dir_entry_2 *de =
				(struct ext2_dir_entry_2 *)(data + offset);

			if (de->rec_len < 8 ||
			    offset + de->rec_len > BLOCK_SIZE)
				break;
			if (de->name_len == 2 && de->name[0] == '.' &&
			    de->name[1] == '.') {
				de->inode = new_parent_ino;
				pgcache_mark_dirty(page);
				if (ext2_sync_dir_page(page) < 0) {
					pgcache_put_page(page);
					return -EIO;
				}
				pgcache_put_page(page);
				return 0;
			}
			offset += de->rec_len;
		}
		pgcache_put_page(page);
	}
	return -ENOENT;
}

static int ext2_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry,
		       unsigned int flags)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	bool old_is_dir;
	bool new_is_dir = false;
	bool cross_dir;
	bool replacing;
	uint8_t ftype;
	int ret;

	if (!old_inode)
		return -ENOENT;
	if (old_dir->i_sb != new_dir->i_sb)
		return -EXDEV;
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	old_is_dir = (old_inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
	cross_dir = old_is_dir && old_dir != new_dir;
	replacing = new_inode != NULL;

	if ((flags & RENAME_NOREPLACE) && new_inode)
		return -EEXIST;
	if (new_inode == old_inode)
		return 0;

	if (new_inode) {
		new_is_dir = (new_inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

		if (!old_is_dir && new_is_dir)
			return -EISDIR;
		if (old_is_dir && !new_is_dir)
			return -ENOTDIR;
		if (new_is_dir && !ext2_dir_is_empty(new_inode))
			return -ENOTEMPTY;
	}

	ftype = ext2_file_type(old_inode->i_mode);
	if (replacing)
		ret = ext2_replace_entry(new_dir, new_dentry,
					 (uint32_t)old_inode->i_ino, ftype);
	else
		ret = ext2_add_entry(new_dir, new_dentry->d_name,
				     new_dentry->d_namelen,
				     (uint32_t)old_inode->i_ino, ftype);
	if (ret < 0)
		return ret;

	if (cross_dir) {
		ret = ext2_set_dotdot(old_inode, (uint32_t)new_dir->i_ino);
		if (ret < 0)
			goto rollback_new;
	}

	ret = ext2_delete_entry(old_dir, old_dentry);
	if (ret < 0) {
		if (cross_dir)
			ext2_set_dotdot(old_inode, (uint32_t)old_dir->i_ino);
		goto rollback_new;
	}

	if (new_inode) {
		if (new_is_dir) {
			new_inode->i_nlink = 0;
			if (new_dir->i_nlink > 0)
				new_dir->i_nlink--;
			ret = ext2_write_inode(new_dir);
			if (ret < 0)
				return ret;
		} else {
			if (new_inode->i_nlink > 0)
				new_inode->i_nlink--;
		}
		ret = ext2_write_inode(new_inode);
		if (ret < 0)
			return ret;
		new_dentry->d_inode = NULL;
		iput(new_inode);
	}

	if (cross_dir) {
		if (old_dir->i_nlink > 0)
			old_dir->i_nlink--;
		new_dir->i_nlink++;
		ret = ext2_write_inode(old_dir);
		if (ret < 0)
			return ret;
		ret = ext2_write_inode(new_dir);
		if (ret < 0)
			return ret;
	}

	return 0;

rollback_new:
	if (replacing)
		ext2_replace_entry(new_dir, new_dentry,
				   (uint32_t)new_inode->i_ino,
				   ext2_file_type(new_inode->i_mode));
	else
		ext2_delete_entry(new_dir, new_dentry);
	return ret;
}

const struct inode_operations ext2_dir_inode_operations = {
	.lookup = ext2_lookup,
	.create = ext2_create,
	.symlink = ext2_symlink,
	.link = ext2_link,
	.unlink = ext2_unlink,
	.mkdir = ext2_mkdir,
	.rmdir = ext2_rmdir,
	.truncate = ext2_truncate_inode,
	.rename = ext2_rename,
};

const struct file_operations ext2_dir_operations = {
	.readdir = ext2_readdir,
};

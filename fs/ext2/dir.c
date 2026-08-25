#include <nuvix/errno.h>
#include <nuvix/page_cache.h>
#include <nuvix/slab.h>
#include <nuvix/tools.h>
#include <nuvix/vfs.h>

#include "ext2.h"

#define EXT2_DIR_REC_LEN(name_len) (((name_len) + 8 + 3) & ~3u)
#define EXT2_DIR_RETRY_LIMIT	   3

typedef int (*ext2_dir_entry_visit_t)(struct ext2_dir_entry_2 *de, void *arg);

static int ext2_validate_dir_entry(const struct ext2_sb_info *sbi,
				   uint8_t *data, uint32_t offset,
				   uint32_t limit,
				   struct ext2_dir_entry_2 **out)
{
	struct ext2_dir_entry_2 *de;
	uint16_t rec_len;

	if (!sbi || !data || !out || limit > BLOCK_SIZE || offset > limit ||
	    offset % sizeof(uint32_t) != 0 ||
	    limit - offset < sizeof(struct ext2_dir_entry_2))
		return -EIO;

	de = (struct ext2_dir_entry_2 *)(data + offset);
	rec_len = de->rec_len;
	if (rec_len < sizeof(struct ext2_dir_entry_2) ||
	    rec_len % sizeof(uint32_t) != 0 || rec_len > limit - offset ||
	    de->name_len > rec_len - sizeof(struct ext2_dir_entry_2) ||
	    EXT2_DIR_REC_LEN(de->name_len) > rec_len ||
	    de->inode > sbi->s_es.s_inodes_count ||
	    ((sbi->s_es.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) &&
	     de->file_type > EXT2_FT_SYMLINK))
		return -EIO;

	*out = de;
	return 0;
}

/* Walks one directory page and calls @visit for every validated entry. */
static int ext2_walk_page_entries(const struct ext2_sb_info *sbi, uint8_t *data,
				  uint32_t limit, ext2_dir_entry_visit_t visit,
				  void *arg)
{
	uint32_t offset = 0;

	if (!visit)
		return -EINVAL;
	while (offset < limit) {
		struct ext2_dir_entry_2 *de;
		int ret =
			ext2_validate_dir_entry(sbi, data, offset, limit, &de);

		if (ret < 0)
			return ret;
		ret = visit(de, arg);
		if (ret != 0)
			return ret;
		offset += de->rec_len;
	}

	return 0;
}

static int ext2_sync_dir_page(struct pgcache *page)
{
	return pgcache_sync_page(page) < 0 ? -EIO : 0;
}

static struct pgcache *ext2_read_inode_page(struct inode *inode,
					    uint32_t lblock)
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
static int ext2_dirent_init(struct ext2_dir_entry_2 *de, uint32_t ino,
			    uint16_t rec_len, const char *name, size_t namelen,
			    uint8_t type)
{
	if (!de || !name || namelen == 0 || namelen > EXT2_NAME_LEN ||
	    rec_len < sizeof(*de) || rec_len > BLOCK_SIZE ||
	    rec_len % sizeof(uint32_t) != 0 ||
	    EXT2_DIR_REC_LEN(namelen) > rec_len)
		return -EIO;
	de->inode = ino;
	de->rec_len = rec_len;
	de->name_len = (uint8_t)namelen;
	de->file_type = type;
	memcpy(de->name, name, namelen);
	return 0;
}

static struct pgcache *ext2_new_inode_page(struct inode *inode, uint32_t lblock)
{
	struct pgcache *page;

	page = pgcache_get_mapping(&inode->i_pages, lblock, PAGE_CACHE_CREATE,
				   NULL);
	if (!page)
		return NULL;

	if (!pgcache_is_uptodate(page)) {
		memset(page_cache_data(page), 0, BLOCK_SIZE);
		pgcache_set_uptodate(page, true);
	}

	return page;
}

static void ext2_unpin_dir_pages(struct pgcache **pages, uint32_t count)
{
	if (!pages)
		return;
	for (uint32_t i = 0; i < count; i++)
		pgcache_put_page(pages[i]);
	kfree(pages);
}

/* Pins every directory page of @dir lockless (page fetches may allocate,
 * which the debug-context gate forbids under s_lock), so a whole-directory
 * scan can run atomically under the lock.  Returns 0 with a pinned array in
 * @out_pages (NULL when the directory has no pages), or a negative errno
 * after releasing any pages pinned so far. */
static int ext2_pin_dir_pages(struct inode *dir, struct pgcache ***out_pages,
			      uint32_t *count)
{
	uint32_t blocks;
	struct pgcache **pages;

	if (!dir || !out_pages || !count)
		return -EINVAL;
	*out_pages = NULL;
	*count = 0;

	if (dir->i_size > EXT2_MAX_FILE_SIZE || dir->i_size % BLOCK_SIZE != 0)
		return -EIO;
	blocks = (uint32_t)(dir->i_size / BLOCK_SIZE);
	if (blocks == 0)
		return 0;

	pages = kmalloc_array(blocks, sizeof(*pages), ALLOC_NOWAIT);
	if (!pages)
		return -ENOMEM;
	for (uint32_t i = 0; i < blocks; i++) {
		uint32_t pblock;
		int ret = ext2_bmap_readonly(dir, i, &pblock);

		pages[i] = NULL;
		if (ret < 0 || !pblock) {
			ext2_unpin_dir_pages(pages, i);
			return ret < 0 ? ret : -EIO;
		}
		pages[i] = ext2_read_inode_page(dir, i);
		if (!pages[i]) {
			ext2_unpin_dir_pages(pages, i);
			return -EIO;
		}
	}
	*out_pages = pages;
	*count = blocks;
	return 0;
}

struct ext2_find_ctx {
	const char *name;
	size_t namelen;
	struct ext2_dir_entry_2 *de;
};

static int ext2_find_visit(struct ext2_dir_entry_2 *de, void *arg)
{
	struct ext2_find_ctx *ctx = arg;

	if (ext2_match(de, ctx->name, ctx->namelen)) {
		ctx->de = de;
		return 1;
	}
	return 0;
}

/* Scans pinned directory pages; s_lock must be held.  Returns 1 when the
 * entry is found, 0 otherwise, or a negative errno. */
static int ext2_find_in_pages(const struct ext2_sb_info *sbi,
			      struct pgcache **pages, uint32_t count,
			      const char *name, size_t namelen,
			      struct pgcache **res_page,
			      struct ext2_dir_entry_2 **res_de)
{
	if (!sbi || !name || namelen == 0 || namelen > EXT2_NAME_LEN)
		return -EINVAL;
	for (uint32_t i = 0; i < count; i++) {
		struct ext2_find_ctx ctx = {.name = name, .namelen = namelen};
		int ret;

		if (!pages || !pages[i])
			return -EIO;
		ret = ext2_walk_page_entries(sbi, page_cache_data(pages[i]),
					     BLOCK_SIZE, ext2_find_visit, &ctx);
		if (ret == 1) {
			if (res_page)
				*res_page = pages[i];
			if (res_de)
				*res_de = ctx.de;
			return 1;
		}
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Unlocked pre-check used by composite create/symlink/mkdir paths.
 * Returns 1 when the entry exists (with the page referenced in @res_page),
 * 0 when it does not, or a negative errno. */
static int ext2_find_entry(struct inode *dir, const char *name, size_t namelen,
			   struct pgcache **res_page)
{
	uint32_t blocks;
	if (!dir || !dir->i_sb || !EXT2_SB(dir->i_sb) || !name ||
	    namelen == 0 || namelen > EXT2_NAME_LEN ||
	    dir->i_size > EXT2_MAX_FILE_SIZE || dir->i_size % BLOCK_SIZE != 0)
		return -EIO;
	blocks = (uint32_t)(dir->i_size / BLOCK_SIZE);

	for (uint32_t lblock = 0; lblock < blocks; lblock++) {
		struct ext2_find_ctx ctx = {.name = name, .namelen = namelen};
		struct pgcache *page;
		uint32_t pblock;
		int ret;

		ret = ext2_bmap_readonly(dir, lblock, &pblock);
		if (ret < 0)
			return ret;
		if (!pblock)
			return -EIO;
		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			return -EIO;

		ret = ext2_walk_page_entries(EXT2_SB(dir->i_sb),
					     page_cache_data(page), BLOCK_SIZE,
					     ext2_find_visit, &ctx);
		if (ret == 1) {
			if (res_page)
				*res_page = page;
			return 1;
		}
		pgcache_put_page(page);
		if (ret < 0)
			return ret;
	}

	return 0;
}

struct ext2_slot_ctx {
	uint16_t need;
	uint32_t ino;
	uint8_t type;
	const char *name;
	size_t namelen;
};

static int ext2_slot_visit(struct ext2_dir_entry_2 *de, void *arg)
{
	struct ext2_slot_ctx *ctx = arg;
	uint16_t used;
	uint16_t spare;

	if (!de->inode && de->rec_len >= ctx->need) {
		int ret = ext2_dirent_init(de, ctx->ino, de->rec_len, ctx->name,
					   ctx->namelen, ctx->type);

		return ret < 0 ? ret : 1;
	}

	used = EXT2_DIR_REC_LEN(de->name_len);
	spare = de->rec_len - used;
	if (spare >= ctx->need) {
		struct ext2_dir_entry_2 *new_de;
		int ret;

		new_de = (struct ext2_dir_entry_2 *)((uint8_t *)de + used);
		ret = ext2_dirent_init(new_de, ctx->ino, spare, ctx->name,
				       ctx->namelen, ctx->type);
		if (ret < 0)
			return ret;
		de->rec_len = used;

		return 1;
	}

	return 0;
}

struct ext2_prev_ctx {
	struct ext2_dir_entry_2 *target;
	struct ext2_dir_entry_2 *prev;
};

static int ext2_prev_visit(struct ext2_dir_entry_2 *de, void *arg)
{
	struct ext2_prev_ctx *ctx = arg;

	if (de == ctx->target)
		return 1;
	ctx->prev = de;
	return 0;
}

struct ext2_nonempty_ctx {
	bool nonempty;
};

static int ext2_nonempty_visit(struct ext2_dir_entry_2 *de, void *arg)
{
	struct ext2_nonempty_ctx *ctx = arg;
	bool dot =
		(de->name_len == 1 && de->name[0] == '.') ||
		(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');

	if (de->inode && !dot) {
		ctx->nonempty = true;
		return 1;
	}
	return 0;
}

/* Directory page mutations pin the pages lockless (fetches may allocate,
 * which is forbidden under a spinlock), then run the whole scan + write as
 * one critical section on the pinned pages.  Page mutations happen under
 * s_lock; device syncs run after it is released.  The extension path maps
 * the new block and re-verifies under s_lock that no other CPU claimed the
 * same index or inserted the name meanwhile. */
static int ext2_add_entry(struct inode *dir, const char *name, size_t namelen,
			  uint32_t ino, uint8_t type)
{
	struct pgcache **pages;
	struct pgcache *page = NULL;
	struct ext2_sb_info *sbi;
	struct ext2_dir_entry_2 *de;
	uint8_t *data;
	uint16_t need;
	uint32_t blocks;
	uint32_t count;
	uint32_t new_block = 0;
	int ret;

	if (!dir || !dir->i_sb || !name || namelen == 0 ||
	    namelen > EXT2_NAME_LEN)
		return -EINVAL;
	sbi = EXT2_SB(dir->i_sb);
	if (!sbi || !ino || ino > sbi->s_es.s_inodes_count ||
	    type > EXT2_FT_SYMLINK)
		return -EIO;
	need = (uint16_t)EXT2_DIR_REC_LEN(namelen);

retry:
	ret = ext2_pin_dir_pages(dir, &pages, &count);
	if (ret < 0)
		return ret;

	spin_lock(&sbi->s_lock);
	if ((uint32_t)(dir->i_size / BLOCK_SIZE) != count) {
		/* Directory grew while pages were being pinned. */
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		goto retry;
	}

	ret = ext2_find_in_pages(sbi, pages, count, name, namelen, NULL, NULL);
	if (ret != 0) {
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return ret == 1 ? -EEXIST : ret;
	}

	for (uint32_t i = 0; i < count; i++) {
		struct ext2_slot_ctx ctx = {
			.need = need,
			.ino = ino,
			.type = type,
			.name = name,
			.namelen = namelen,
		};

		if (!pages || !pages[i]) {
			spin_unlock(&sbi->s_lock);
			ext2_unpin_dir_pages(pages, count);
			return -EIO;
		}
		ret = ext2_walk_page_entries(sbi, page_cache_data(pages[i]),
					     BLOCK_SIZE, ext2_slot_visit, &ctx);
		if (ret == 1) {
			pgcache_mark_dirty(pages[i]);
			spin_unlock(&sbi->s_lock);
			if (ext2_sync_dir_page(pages[i]) < 0) {
				ext2_unpin_dir_pages(pages, count);
				return -EIO;
			}
			ext2_unpin_dir_pages(pages, count);
			return 0;
		}
		if (ret < 0) {
			spin_unlock(&sbi->s_lock);
			ext2_unpin_dir_pages(pages, count);
			return ret;
		}
	}
	spin_unlock(&sbi->s_lock);

	/* No free slot: extend the directory at index @count. */
	if (dir->i_size > EXT2_MAX_FILE_SIZE - BLOCK_SIZE) {
		ext2_unpin_dir_pages(pages, count);
		return -EFBIG;
	}
	blocks = count;
	ret = ext2_bmap(dir, blocks, true, &new_block);
	if (ret < 0) {
		ext2_unpin_dir_pages(pages, count);
		return ret;
	}
	if (!new_block) {
		ext2_unpin_dir_pages(pages, count);
		return -ENOSPC;
	}

	page = pgcache_get_mapping(&dir->i_pages, blocks, PAGE_CACHE_READ,
				   &ret);
	if (!page) {
		ext2_unpin_dir_pages(pages, count);
		return ret < 0 ? ret : -EIO;
	}

	spin_lock(&sbi->s_lock);
	if ((uint32_t)(dir->i_size / BLOCK_SIZE) != blocks) {
		/* Another CPU extended to this index first; its entry lives
		 * in this page.  Re-scan from scratch. */
		spin_unlock(&sbi->s_lock);
		pgcache_put_page(page);
		ext2_unpin_dir_pages(pages, count);
		goto retry;
	}
	ret = ext2_find_in_pages(sbi, pages, count, name, namelen, NULL, NULL);
	if (ret != 0) {
		spin_unlock(&sbi->s_lock);
		pgcache_put_page(page);
		ext2_unpin_dir_pages(pages, count);
		return ret == 1 ? -EEXIST : ret;
	}

	data = page_cache_data(page);
	memset(data, 0, BLOCK_SIZE);
	de = (struct ext2_dir_entry_2 *)data;
	ret = ext2_dirent_init(de, ino, BLOCK_SIZE, name, namelen, type);
	if (ret < 0) {
		spin_unlock(&sbi->s_lock);
		pgcache_put_page(page);
		ext2_unpin_dir_pages(pages, count);
		return ret;
	}
	pgcache_mark_dirty(page);
	dir->i_size += BLOCK_SIZE;
	spin_unlock(&sbi->s_lock);

	if (ext2_sync_dir_page(page) < 0) {
		pgcache_put_page(page);
		ext2_unpin_dir_pages(pages, count);
		return -EIO;
	}
	pgcache_put_page(page);
	ext2_unpin_dir_pages(pages, count);
	return ext2_write_inode(dir);
}

static int ext2_delete_entry(struct inode *dir, struct dentry *dentry)
{
	struct pgcache **pages;
	struct pgcache *found_page = NULL;
	struct ext2_dir_entry_2 *de;
	struct ext2_prev_ctx pctx;
	struct ext2_sb_info *sbi;
	uint32_t count;
	int ret;

	if (!dir || !dir->i_sb || !dentry)
		return -EINVAL;
	sbi = EXT2_SB(dir->i_sb);

	ret = ext2_pin_dir_pages(dir, &pages, &count);
	if (ret < 0)
		return ret;

	spin_lock(&sbi->s_lock);
	if ((uint32_t)(dir->i_size / BLOCK_SIZE) != count) {
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return -ENOENT;
	}

	ret = ext2_find_in_pages(sbi, pages, count, dentry->d_name,
				 dentry->d_namelen, &found_page, &de);
	if (ret != 1) {
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return ret == 0 ? -ENOENT : ret;
	}

	pctx.target = de;
	pctx.prev = NULL;
	ret = ext2_walk_page_entries(sbi, page_cache_data(found_page),
				     BLOCK_SIZE, ext2_prev_visit, &pctx);
	if (ret != 1) {
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return ret < 0 ? ret : -ENOENT;
	}
	if (pctx.prev)
		pctx.prev->rec_len += de->rec_len;
	else
		de->inode = 0;

	pgcache_mark_dirty(found_page);
	spin_unlock(&sbi->s_lock);

	if (ext2_sync_dir_page(found_page) < 0) {
		ext2_unpin_dir_pages(pages, count);
		return -EIO;
	}
	ext2_unpin_dir_pages(pages, count);
	return 0;
}

static int ext2_replace_entry(struct inode *dir, struct dentry *dentry,
			      uint32_t ino, uint8_t type)
{
	struct pgcache **pages;
	struct pgcache *found_page = NULL;
	struct ext2_dir_entry_2 *de;
	struct ext2_sb_info *sbi;
	uint32_t count;
	int ret;

	if (!dir || !dir->i_sb || !dentry)
		return -EINVAL;
	sbi = EXT2_SB(dir->i_sb);

	ret = ext2_pin_dir_pages(dir, &pages, &count);
	if (ret < 0)
		return ret;

	spin_lock(&sbi->s_lock);
	if ((uint32_t)(dir->i_size / BLOCK_SIZE) != count) {
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return -ENOENT;
	}

	ret = ext2_find_in_pages(sbi, pages, count, dentry->d_name,
				 dentry->d_namelen, &found_page, &de);
	if (ret != 1) {
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return ret == 0 ? -ENOENT : ret;
	}

	de->inode = ino;
	de->file_type = type;
	pgcache_mark_dirty(found_page);
	spin_unlock(&sbi->s_lock);

	if (ext2_sync_dir_page(found_page) < 0) {
		ext2_unpin_dir_pages(pages, count);
		return -EIO;
	}
	ext2_unpin_dir_pages(pages, count);
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
	struct pgcache **pages;
	struct ext2_dir_entry_2 *de;
	struct ext2_sb_info *sbi;
	struct inode *inode;
	uint32_t count;
	uint32_t ino = 0;
	int ret;

	if (!dir || !dentry)
		return NULL;
	if ((dir->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
		return NULL;
	if (!dir->i_sb)
		return NULL;
	sbi = EXT2_SB(dir->i_sb);

	ret = ext2_pin_dir_pages(dir, &pages, &count);
	if (ret < 0)
		return ERR_PTR(ret);

	spin_lock(&sbi->s_lock);
	ret = ext2_find_in_pages(sbi, pages, count, dentry->d_name,
				 dentry->d_namelen, NULL, &de);
	if (ret == 1)
		ino = de->inode;
	/* The inode number is snapshotted under the lock; the pinned pages
	 * are released before iget so no page ref is carried across I/O. */
	spin_unlock(&sbi->s_lock);
	ext2_unpin_dir_pages(pages, count);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret == 0)
		return NULL;

	inode = iget(dir->i_sb, ino);
	if (!inode)
		return ERR_PTR(-EIO);

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

	ret = ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page);
	if (ret == 1) {
		pgcache_put_page(page);
		return -EEXIST;
	}
	if (ret < 0)
		return ret;

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
	ret = ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page);
	if (ret == 1) {
		pgcache_put_page(page);
		return -EEXIST;
	}
	if (ret < 0)
		return ret;

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

/* The new inode is unpublished until add_entry links it, so no other CPU
 * can race this setup; the bmap/write_inode leaves self-lock their shared
 * state. */
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
	ret = ext2_dirent_init(de, (uint32_t)inode->i_ino, EXT2_DIR_REC_LEN(1),
			       ".", 1, EXT2_FT_DIR);
	if (ret < 0) {
		pgcache_put_page(page);
		return ret;
	}

	de = (struct ext2_dir_entry_2 *)(data + de->rec_len);
	ret = ext2_dirent_init(de, (uint32_t)parent->i_ino,
			       BLOCK_SIZE - EXT2_DIR_REC_LEN(1), "..", 2,
			       EXT2_FT_DIR);
	if (ret < 0) {
		pgcache_put_page(page);
		return ret;
	}

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

	ret = ext2_find_entry(dir, dentry->d_name, dentry->d_namelen, &page);
	if (ret == 1) {
		pgcache_put_page(page);
		return -EEXIST;
	}
	if (ret < 0)
		return ret;

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

static int ext2_dir_is_empty(struct inode *inode)
{
	struct pgcache **pages;
	struct ext2_sb_info *sbi;
	uint32_t count;
	int attempt;

	if (!inode || !inode->i_sb)
		return -EINVAL;
	sbi = EXT2_SB(inode->i_sb);

	for (attempt = 0; attempt < EXT2_DIR_RETRY_LIMIT; attempt++) {
		bool empty = true;
		int ret = ext2_pin_dir_pages(inode, &pages, &count);

		if (ret < 0)
			return ret;

		spin_lock(&sbi->s_lock);
		if ((uint32_t)(inode->i_size / BLOCK_SIZE) != count) {
			/* Directory changed while pages were being pinned. */
			spin_unlock(&sbi->s_lock);
			ext2_unpin_dir_pages(pages, count);
			continue;
		}
		for (uint32_t i = 0; i < count; i++) {
			struct ext2_nonempty_ctx ctx = {.nonempty = false};
			int wret;

			if (!pages[i]) {
				spin_unlock(&sbi->s_lock);
				ext2_unpin_dir_pages(pages, count);
				return -EIO;
			}
			wret = ext2_walk_page_entries(
				sbi, page_cache_data(pages[i]), BLOCK_SIZE,
				ext2_nonempty_visit, &ctx);
			if (ctx.nonempty) {
				empty = false;
				break;
			}
			if (wret < 0) {
				spin_unlock(&sbi->s_lock);
				ext2_unpin_dir_pages(pages, count);
				return wret;
			}
		}
		spin_unlock(&sbi->s_lock);
		ext2_unpin_dir_pages(pages, count);
		return empty ? 1 : 0;
	}

	/* Repeatedly changed under us: fail conservatively as non-empty. */
	return 0;
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
	ret = ext2_dir_is_empty(inode);
	if (ret < 0)
		return ret;
	if (ret == 0)
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

/* Per-entry critical sections: the page is fetched lockless (fetch may
 * allocate), then the entry parse and the f_pos advance run under s_lock
 * so concurrent readers of one dir fd do not skip or duplicate entries.
 * filldir writes into a caller kernel buffer, never user memory. */
static int ext2_readdir(struct file *file, void *ctx, filldir_t filldir)
{
	struct inode *dir;
	struct ext2_sb_info *sbi;

	if (!file || !filldir)
		return -EINVAL;
	dir = file->f_inode;
	if (!dir || !dir->i_sb)
		return -EINVAL;
	sbi = EXT2_SB(dir->i_sb);
	if (!sbi || file->f_pos < 0)
		return -EIO;

	while ((uint64_t)file->f_pos < dir->i_size) {
		uint32_t lblock =
			(uint32_t)((uint64_t)file->f_pos / BLOCK_SIZE);
		uint32_t offset =
			(uint32_t)((uint64_t)file->f_pos % BLOCK_SIZE);
		struct pgcache *page;
		struct ext2_dir_entry_2 *de;
		uint8_t *data;
		loff_t next_pos;
		uint32_t pblock;
		int ret;

		ret = ext2_bmap_readonly(dir, lblock, &pblock);
		if (ret < 0)
			return ret;
		if (!pblock)
			return -EIO;

		page = ext2_read_inode_page(dir, lblock);
		if (!page)
			return -EIO;

		spin_lock(&sbi->s_lock);
		data = page_cache_data(page);
		ret = ext2_validate_dir_entry(sbi, data, offset, BLOCK_SIZE,
					      &de);
		if (ret < 0) {
			spin_unlock(&sbi->s_lock);
			pgcache_put_page(page);
			return ret;
		}

		next_pos = file->f_pos + de->rec_len;
		if (de->inode) {
			int fill_ret =
				filldir(ctx, de->name, de->name_len, de->inode,
					de->file_type, next_pos);

			if (fill_ret < 0) {
				spin_unlock(&sbi->s_lock);
				pgcache_put_page(page);
				return fill_ret;
			}
		}

		file->f_pos = next_pos;
		spin_unlock(&sbi->s_lock);
		pgcache_put_page(page);
	}

	return 0;
}

static int ext2_set_dotdot(struct inode *dir, uint32_t new_parent_ino)
{
	struct pgcache **pages;
	struct pgcache *found_page = NULL;
	struct ext2_dir_entry_2 *de;
	struct ext2_sb_info *sbi;
	uint32_t count;
	int attempt;

	if (!dir || !dir->i_sb)
		return -EINVAL;
	sbi = EXT2_SB(dir->i_sb);
	if (!sbi || !new_parent_ino ||
	    new_parent_ino > sbi->s_es.s_inodes_count)
		return -EIO;

	for (attempt = 0; attempt < EXT2_DIR_RETRY_LIMIT; attempt++) {
		int ret = ext2_pin_dir_pages(dir, &pages, &count);

		if (ret < 0)
			return ret;

		spin_lock(&sbi->s_lock);
		if ((uint32_t)(dir->i_size / BLOCK_SIZE) != count) {
			/* Directory changed while pages were being pinned. */
			spin_unlock(&sbi->s_lock);
			ext2_unpin_dir_pages(pages, count);
			continue;
		}

		ret = ext2_find_in_pages(sbi, pages, count, "..", 2,
					 &found_page, &de);
		if (ret != 1) {
			spin_unlock(&sbi->s_lock);
			ext2_unpin_dir_pages(pages, count);
			return ret == 0 ? -ENOENT : ret;
		}

		de->inode = new_parent_ino;
		pgcache_mark_dirty(found_page);
		spin_unlock(&sbi->s_lock);

		if (ext2_sync_dir_page(found_page) < 0) {
			ext2_unpin_dir_pages(pages, count);
			return -EIO;
		}
		ext2_unpin_dir_pages(pages, count);
		return 0;
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
		if (new_is_dir) {
			ret = ext2_dir_is_empty(new_inode);
			if (ret < 0)
				return ret;
			if (ret == 0)
				return -ENOTEMPTY;
		}
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

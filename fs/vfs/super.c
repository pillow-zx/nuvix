/*
 * fs/vfs/super.c - VFS 超级块管理
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/slab.h>
#include <nuvix/vfs.h>

#define NR_FILESYSTEMS 8

static struct file_system_type *fs_types[NR_FILESYSTEMS];

int register_filesystem(struct file_system_type *fs_type)
{
	if (!fs_type || !fs_type->name || !fs_type->mount)
		return -EINVAL;

	for (uint32_t i = 0; i < NR_FILESYSTEMS; i++) {
		if (fs_types[i] &&
		    strcmp(fs_types[i]->name, fs_type->name) == 0)
			return -EINVAL;
	}

	for (uint32_t i = 0; i < NR_FILESYSTEMS; i++) {
		if (!fs_types[i]) {
			fs_types[i] = fs_type;
			fs_type->next = NULL;
			return 0;
		}
	}

	return -ENFILE;
}

struct file_system_type *get_filesystem_type(const char *name)
{
	if (!name)
		return NULL;

	for (uint32_t i = 0; i < NR_FILESYSTEMS; i++) {
		if (fs_types[i] && strcmp(fs_types[i]->name, name) == 0)
			return fs_types[i];
	}

	return NULL;
}

struct file_system_type *get_next_filesystem_type(struct file_system_type *prev)
{
	bool found = prev == NULL;

	for (uint32_t i = 0; i < NR_FILESYSTEMS; i++) {
		if (!fs_types[i])
			continue;
		if (found)
			return fs_types[i];
		if (fs_types[i] == prev)
			found = true;
	}

	return NULL;
}

struct super_block *super_alloc(struct file_system_type *fs_type, dev_t dev)
{
	struct super_block *sb = kmalloc(sizeof(*sb), ALLOC_NOWAIT);
	if (!sb)
		return NULL;

	memset(sb, 0, sizeof(*sb));
	sb->s_dev = dev;
	sb->s_type = fs_type;
	INIT_LIST_HEAD(&sb->s_inodes);

	return sb;
}

/*
 * Failable superblock teardown transaction.  Must run while the owning mount
 * is still attached and quiesced: all external path references are gone, so
 * dentry/inode refcounts reflect only cache residency.
 *
 * Phase 1 retires the root dentry subtree child-first; any pinned dentry
 * aborts with -EBUSY leaving everything published.
 * Phase 2 evicts every inode on sb->s_inodes; each eviction may fail (page
 * cache pins, I/O error) and leaves its object intact for a retry.
 * On any failure the sticky s_error records the first failure cause and the
 * transaction returns nonzero without releasing filesystem-private state.
 *
 * Only after this succeeds may vfs_super_destroy() run the final,
 * failure-free release.
 */
int vfs_super_teardown(struct super_block *sb)
{
	int first_error = 0;

	if (!sb)
		return -EINVAL;

	/* Detach the root dentry from active duty before walking inodes so
	 * no lookup can resurrect references mid-transaction. */
	if (sb->s_root) {
		int ret = dcache_retire_subtree(sb->s_root);

		if (ret < 0) {
			if (!first_error)
				first_error = ret;
			goto out_fail;
		}
		sb->s_root = NULL;
	}

	while (!list_empty(&sb->s_inodes)) {
		struct inode *inode = list_first_entry(&sb->s_inodes,
						       struct inode, i_sb_list);
		int ret = inode_evict(inode);

		if (ret < 0) {
			if (!first_error)
				first_error = ret;
			break;
		}
	}

out_fail:
	if (first_error < 0) {
		if (!sb->s_error)
			sb->s_error = first_error;
		return first_error;
	}
	return 0;
}

/* Aborted transaction: a successful teardown was followed by a later failure
 * (device page-cache discard), so the sb must return to service.  Objects
 * were fully retired and cannot come back; the sb keeps operating with an
 * empty cache view, which is semantically valid (like a freshly mounted fs).
 * Only the error is recorded for the next sync report. */
void vfs_super_mark_aborted(struct super_block *sb)
{
	if (!sb)
		return;
	if (!sb->s_error)
		sb->s_error = -EIO;
}

void vfs_super_destroy(struct super_block *sb)
{
	struct dentry *root;

	if (!sb)
		return;

	/* Final release after a successful teardown leaves s_root NULL and
	 * s_inodes empty, so the loops below are no-ops there.  They remain
	 * as best-effort forced cleanup for failed mount attachments, which
	 * never published anything and cannot propagate errors. */
	root = sb->s_root;
	if (root) {
		dcache_invalidate(root);
		dput(root);
		sb->s_root = NULL;
	}

	while (!list_empty(&sb->s_inodes)) {
		struct inode *inode = list_first_entry(&sb->s_inodes,
						       struct inode, i_sb_list);

		inode_forget(inode);
	}

	if (sb->s_op && sb->s_op->put_super)
		sb->s_op->put_super(sb);
	kfree(sb);
}

void vfs_init(void)
{
	icache_init();
	dcache_init();

	for (uint32_t i = 0; i < NR_FILESYSTEMS; i++)
		fs_types[i] = NULL;
}

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

void vfs_init(void)
{
	icache_init();
	dcache_init();

	for (uint32_t i = 0; i < NR_FILESYSTEMS; i++)
		fs_types[i] = NULL;
}

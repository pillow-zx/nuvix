#ifndef _NUVIX_FS_EXT2_EXT2_H
#define _NUVIX_FS_EXT2_EXT2_H

/*
 * fs/ext2/ext2.h - EXT2 磁盘格式定义
 */

#include <nuvix/blkdev.h>
#include <nuvix/compiler.h>
#include <nuvix/fs.h>
#include <nuvix/spinlock.h>
#include <nuvix/types.h>

#define EXT2_SUPER_MAGIC	 0xef53
#define EXT2_ROOT_INO		 2
#define EXT2_GOOD_OLD_REV	 0
#define EXT2_DYNAMIC_REV	 1
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_NAME_LEN		 255

#define EXT2_OS_LINUX 0

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE	    0x0002

#define EXT2_SUPER_OFFSET		  1024u
#define EXT2_BGDT_BLOCK(first_data_block) ((first_data_block) + 1u)

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK	 12
#define EXT2_DIND_BLOCK	 13
#define EXT2_TIND_BLOCK	 14
#define EXT2_N_BLOCKS	 15

#define EXT2_MAX_FILE_SIZE  ((uint64_t)UINT32_MAX)
#define EXT2_MAX_FILE_INDEX ((EXT2_MAX_FILE_SIZE - 1) / BLOCK_SIZE)

#define EXT2_FT_UNKNOWN	 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR	 2
#define EXT2_FT_CHRDEV	 3
#define EXT2_FT_BLKDEV	 4
#define EXT2_FT_FIFO	 5
#define EXT2_FT_SOCK	 6
#define EXT2_FT_SYMLINK	 7

#define EXT2_S_IFMT   0xf000
#define EXT2_S_IFSOCK 0xc000
#define EXT2_S_IFLNK  0xa000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000

struct __packed ext2_super_block {
	uint32_t s_inodes_count;
	uint32_t s_blocks_count;
	uint32_t s_r_blocks_count;
	uint32_t s_free_blocks_count;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_frag_size;
	uint32_t s_blocks_per_group;
	uint32_t s_frags_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;
	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t s_uuid[16];
	char s_volume_name[16];
	char s_last_mounted[64];
	uint32_t s_algorithm_usage_bitmap;
	uint8_t s_prealloc_blocks;
	uint8_t s_prealloc_dir_blocks;
	uint16_t s_padding1;
	uint8_t s_journal_uuid[16];
	uint32_t s_journal_inum;
	uint32_t s_journal_dev;
	uint32_t s_last_orphan;
	uint32_t s_hash_seed[4];
	uint8_t s_def_hash_version;
	uint8_t s_reserved_char_pad;
	uint16_t s_reserved_word_pad;
	uint32_t s_default_mount_opts;
	uint32_t s_first_meta_bg;
	uint32_t s_reserved[190];
};

struct __packed ext2_group_desc {
	uint32_t bg_block_bitmap;
	uint32_t bg_inode_bitmap;
	uint32_t bg_inode_table;
	uint16_t bg_free_blocks_count;
	uint16_t bg_free_inodes_count;
	uint16_t bg_used_dirs_count;
	uint16_t bg_pad;
	uint32_t bg_reserved[3];
};

struct __packed ext2_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	uint32_t i_dtime;
	uint16_t i_gid;
	uint16_t i_links_count;
	uint32_t i_blocks;
	uint32_t i_flags;
	uint32_t i_osd1;
	uint32_t i_block[EXT2_N_BLOCKS];
	uint32_t i_generation;
	uint32_t i_file_acl;
	uint32_t i_dir_acl;
	uint32_t i_faddr;
	uint8_t i_osd2[12];
};

struct __packed ext2_dir_entry_2 {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[];
};

struct ext2_sb_info {
	/* Serializes all on-disk state mutations: superblock counters, group
	 * descriptors, bitmaps, inode table blocks, and directory entry
	 * pages.  Held at rank 24, below the page cache (25) and virtio
	 * submit (26).  Nested internally only via the *_locked variants of
	 * ext2_bmap/ext2_write_inode/ext2_alloc_block. */
	spinlock_t s_lock;
	struct ext2_super_block s_es;
	struct ext2_group_desc *s_group_desc;
	uint32_t s_groups_count;
	uint32_t s_inode_size;
	uint32_t s_inodes_per_group;
	uint32_t s_blocks_per_group;
	uint32_t s_first_data_block;
};

struct ext2_inode_info {
	struct ext2_inode raw_inode;
};

extern const struct inode_operations ext2_dir_inode_operations;
extern const struct inode_operations ext2_symlink_inode_operations;
extern const struct file_operations ext2_dir_operations;
extern const struct file_operations ext2_file_operations;
extern const struct page_mapping_ops ext2_inode_mapping_ops;

static inline struct ext2_sb_info *EXT2_SB(struct super_block *sb)
{
	return (struct ext2_sb_info *)sb->s_private;
}

static inline struct ext2_inode_info *EXT2_I(struct inode *inode)
{
	return (struct ext2_inode_info *)inode->i_private;
}

static inline uint32_t ext2_super_blocknr(uint32_t block_size)
{
	return EXT2_SUPER_OFFSET / block_size;
}

static inline uint32_t ext2_super_offset(uint32_t block_size)
{
	return EXT2_SUPER_OFFSET % block_size;
}

int ext2_init(void);

int ext2_read_inode(struct inode *inode);
int ext2_write_inode(struct inode *inode);
int ext2_datasync_inode(struct inode *inode);
void ext2_init_inode_ops(struct inode *inode);
void ext2_free_inode_blocks(struct inode *inode);
int ext2_bmap(struct inode *inode, uint32_t block, bool create,
	      uint32_t *mapped);
/* The *_locked variants expect s_lock held; plain entry points self-lock. */
int ext2_bmap_locked(struct inode *inode, uint32_t block, bool create,
		     uint32_t *mapped);
int ext2_write_inode_locked(struct inode *inode);
uint32_t ext2_alloc_block_locked(struct inode *inode);
uint32_t ext2_bmap_readonly(struct inode *inode, uint32_t block);
int ext2_truncate_inode(struct inode *inode, uint64_t size);

uint32_t ext2_alloc_block(struct inode *inode);
void ext2_free_block(struct super_block *sb, uint32_t block);
uint32_t ext2_alloc_inode(struct super_block *sb, uint16_t mode);
void ext2_free_inode(struct super_block *sb, uint32_t ino);

ssize_t ext2_read_file(struct inode *inode, char *buf, size_t count,
		       loff_t pos);
ssize_t ext2_write_file(struct inode *inode, const char *buf, size_t count,
			loff_t pos);

#endif

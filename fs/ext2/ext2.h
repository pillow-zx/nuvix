#ifndef _NUVIX_FS_EXT2_H
#define _NUVIX_FS_EXT2_H

#include <nuvix/assert.h>
#include <nuvix/compiler.h>
#include <nuvix/fs.h>
#include <nuvix/mapping.h>

#define EXT2_SUPER_MAGIC               0xEF53
#define EXT2_SUPER_OFFSET              1024

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

#define EXT2_ROOT_INO                  2
#define EXT2_GOOD_OLD_INODE_SIZE       128
#define EXT2_GOOD_OLD_FIRST_INO        11
#define EXT2_LINK_MAX                  32000

#define EXT2_NDIR_BLOCKS               12
#define EXT2_IND_BLOCK                 12
#define EXT2_DIND_BLOCK                13
#define EXT2_TIND_BLOCK                14
#define EXT2_N_BLOCKS                  15

#define EXT2_FT_UNKNOWN                0
#define EXT2_FT_REG_FILE               1
#define EXT2_FT_DIR                    2
#define EXT2_FT_CHRDEV                 3
#define EXT2_FT_BLKDEV                 4
#define EXT2_FT_FIFO                   5
#define EXT2_FT_SOCK                   6
#define EXT2_FT_SYMLINK                7
#define EXT2_FT_MAX                    8

struct ext2_super_block {
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
} __packed;

static_assert(sizeof(struct ext2_super_block) == 104,
              "invalid ext2 superblock layout");

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __packed;

static_assert(sizeof(struct ext2_group_desc) == 32,
              "invalid ext2 group descriptor layout");

struct ext2_inode {
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
} __packed;

static_assert(sizeof(struct ext2_inode) == 128, "invalid ext2 inode layout");

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __packed;

static_assert(sizeof(struct ext2_dirent) == 8,
              "invalid ext2 directory entry layout");

struct ext2_sb_info {
    struct mutex mutex;
    struct mapping mapping;
    struct cache_page *s_sbh;
    struct ext2_super_block *s_es;
    uint32_t block_size;
    uint32_t inode_size;
};

struct ext2_inode_info {
    struct mapping mapping;
    uint32_t data[EXT2_N_BLOCKS];
    struct inode vfs_inode;
};

static __always_inline struct ext2_sb_info *ext2_sb_info(struct super_block *sb)
{
    return sb->s_fs_info;
}

static __always_inline struct ext2_inode_info *ext2_i(struct inode *inode)
{
    return container_of(inode, struct ext2_inode_info, vfs_inode);
}

struct cache_page *ext2_get_grp_desc(struct ext2_sb_info *sbi, uint32_t grp,  uint32_t *off);

extern const struct super_operations ext2_super_operations;
extern const struct file_operations ext2_dir_operations;
extern const struct file_operations ext2_file_operations;
extern const struct mapping_ops ext2_file_mapping_operations;

struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry);
int ext2_create(struct inode *dir, struct dentry *dentry);
struct dentry *ext2_mkdir(struct inode *dir, struct dentry *dentry);
int ext2_unlink(struct inode *dir, struct dentry *dentry);
int ext2_rmdir(struct inode *dir, struct dentry *dentry);
int ext2_symlink(struct inode *dir, struct dentry *dentry, const char *target);
ssize_t ext2_readlink(struct inode *inode, char *buf, size_t len);
int ext2_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
int ext2_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);
loff_t ext2_llseek(struct file *file, loff_t off, int whence);

struct inode *ext2_iget(struct super_block *sb, uint32_t ino);
struct cache_page *ext2_get_inode_page(struct inode *inode, uint32_t *off);
void ext2_write_inode(struct inode *inode, struct cache_page *cp, uint32_t off);

int ext2_bmap(struct inode *inode, uint32_t lbn, uint32_t *blk);
int ext2_bmap_create(struct inode *inode, uint32_t lbn, uint32_t *blk);
int ext2_truncate_blocks(struct inode *inode, uint64_t keep);
void ext2_free_blocks(struct inode *inode);

int ext2_alloc_block(struct super_block *sb, uint32_t *blk);
int ext2_free_block(struct super_block *sb, uint32_t blk);

struct inode *ext2_alloc_inode(struct inode *dir, unsigned int mode);
void ext2_free_inode(struct inode *inode);

int ext2_find_ino(struct inode *dir, const struct dentry *dentry, uint32_t *ino);
struct cache_page *ext2_find_dirent(struct inode *dir, const char *name, uint32_t len, uint32_t *off);
int ext2_add_dirent(struct inode *dir, const char *name, uint32_t len, struct inode *inode);
void ext2_set_dirent(struct inode *dir, struct cache_page *cp, uint32_t off, struct inode *inode);
void ext2_delete_dirent(struct inode *dir, struct cache_page *cp, uint32_t off);

int ext2_init_dir(struct inode *dir, struct inode *parent);
int ext2_dir_is_empty(struct inode *dir);

#endif /* ifndef _NUVIX_FS_EXT2_H */

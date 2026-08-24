#ifndef _NUVIX_FS_H
#define _NUVIX_FS_H

/**
 * @file fs.h
 * @brief 虚拟文件系统核心对象、操作表与通用文件 API。
 */

#include <nuvix/page_mapping.h>
#include <nuvix/hlist.h>
#include <nuvix/list.h>
#include <nuvix/mutex.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>
#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <nuvix/wait.h>
#include <uapi/dirent.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>
#include <uapi/stat.h>

struct task_struct;
struct trap_frame;
struct file;
struct inode;
struct dentry;
struct super_block;
struct statfs64;
struct task_wait;

/**
 * @def VFS_NAME_MAX
 * @brief Maximum single path-component length accepted by VFS.
 */
#define VFS_NAME_MAX 255

/**
 * @def VFS_PATH_MAX
 * @brief Maximum absolute or relative path string length copied from userspace.
 */
#define VFS_PATH_MAX 4096

/** @def KERN_STDIN Kernel-reserved stdin file descriptor number. */
#define KERN_STDIN 0

/** @def KERN_STDOUT Kernel-reserved stdout file descriptor number. */
#define KERN_STDOUT 1

/** @def KERN_STDERR Kernel-reserved stderr file descriptor number. */
#define KERN_STDERR 2

/**
 * @def NR_OPEN
 * @brief Per-task fdtable size and poll/select descriptor ceiling.
 */
#define NR_OPEN 32

/** @def FMODE_READ File object permits read operations. */
#define FMODE_READ 0x1

/** @def FMODE_WRITE File object permits write operations. */
#define FMODE_WRITE 0x2

/** @def VFS_MODE_CHMOD_MASK Special and permission bits set by chmod. */
#define VFS_MODE_CHMOD_MASK 07777

/** @def VFS_MODE_SETID_MASK Set-user-ID and set-group-ID mode bits. */
#define VFS_MODE_SETID_MASK 06000

/** @def VFS_ATTR_MODE Update the inode permission and special mode bits. */
#define VFS_ATTR_MODE 0x0001

/** @def VFS_ATTR_UID Update the inode owner uid. */
#define VFS_ATTR_UID 0x0002

/** @def VFS_ATTR_GID Update the inode owner gid. */
#define VFS_ATTR_GID 0x0004

/** @def VFS_ATTR_ALL All attributes supported by vfs_inode_setattr(). */
#define VFS_ATTR_ALL (VFS_ATTR_MODE | VFS_ATTR_UID | VFS_ATTR_GID)

/**
 * @struct vfs_inode_attrs
 * @brief Caller-requested inode metadata changes owned by VFS.
 *
 * @c valid selects which fields are applied. A mode update changes only the
 * special and permission bits; VFS preserves the inode file type.
 */
struct vfs_inode_attrs {
	uint32_t valid;
	uint32_t mode;
	uid_t uid;
	gid_t gid;
};

/**
 * @typedef filldir_t
 * @brief Callback used by filesystem readdir implementations.
 * @param ctx Caller-owned output context.
 * @param name Directory entry name.
 * @param namelen Length of @p name in bytes.
 * @param ino Filesystem inode number.
 * @param type Linux d_type-compatible directory entry type.
 * @param off Directory stream offset to report for this entry.
 * @return 0 to continue, or a negative errno to stop.
 */
typedef int (*filldir_t)(void *ctx, const char *name, size_t namelen,
			 uint64_t ino, uint8_t type, loff_t off);

/**
 * @struct super_operations
 * @brief Filesystem-wide operations hanging off a super_block.
 *
 * @par Fields
 * - @c read_inode: Populate an inode from persistent filesystem metadata.
 * - @c write_inode: Write dirty inode metadata back to persistent storage.
 * - @c datasync_inode: Write metadata required to retrieve file data after
 * `fdatasync`; when omitted, VFS falls back to @c write_inode.
 * - @c evict_inode: Release filesystem-private inode state at final eviction.
 * - @c sync_fs: Flush filesystem-wide dirty state.
 * - @c statfs: Fill Linux statfs64-compatible filesystem statistics.
 */
struct super_operations {
	int (*read_inode)(struct inode *inode);
	int (*write_inode)(struct inode *inode);
	int (*datasync_inode)(struct inode *inode);
	void (*evict_inode)(struct inode *inode);
	int (*sync_fs)(struct super_block *sb);
	int (*statfs)(struct super_block *sb, struct statfs64 *buf);
};

/**
 * @struct inode_operations
 * @brief Namespace and metadata operations implemented by a filesystem.
 *
 * @par Fields
 * - @c lookup: Resolve @p dentry under directory inode @p dir.
 * - @c create: Create a regular file with VFS-applied mode bits.
 * - @c symlink: Create a symbolic link storing @p target.
 * - @c link: Add a hard link from @p new_dentry to @p old_dentry.
 * - @c unlink: Remove a non-directory entry from @p dir.
 * - @c mkdir: Create a directory entry and backing directory inode.
 * - @c rmdir: Remove an empty directory entry.
 * - @c readlink: Copy symlink target bytes into @p buf.
 * - @c truncate: Resize file contents and metadata to @p size.
 * - @c fallocate: Allocate blocks for a file range according to Linux fallocate
 * flags.
 * - @c rename: Move or exchange namespace entries according to renameat2 flags.
 */
struct inode_operations {
	struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry);
	int (*create)(struct inode *dir, struct dentry *dentry, uint32_t mode);
	int (*symlink)(struct inode *dir, struct dentry *dentry,
		       const char *target);
	int (*link)(struct dentry *old_dentry, struct inode *dir,
		    struct dentry *new_dentry);
	int (*unlink)(struct inode *dir, struct dentry *dentry);
	int (*mkdir)(struct inode *dir, struct dentry *dentry, uint32_t mode);
	int (*rmdir)(struct inode *dir, struct dentry *dentry);
	int (*readlink)(struct inode *inode, char *buf, size_t size);
	int (*truncate)(struct inode *inode, uint64_t size);
	int (*fallocate)(struct inode *inode, int mode, uint64_t offset,
			 uint64_t len);
	int (*rename)(struct inode *old_dir, struct dentry *old_dentry,
		      struct inode *new_dir, struct dentry *new_dentry,
		      unsigned int flags);
};

/**
 * @struct file_operations
 * @brief Open-file operations used by VFS read/write/poll/ioctl paths.
 *
 * @par Fields
 * - @c read: Read from an explicit file position into a kernel buffer.
 * - @c write: Write from a kernel buffer at an explicit file position.
 * - @c llseek: Implement SEEK_* repositioning for this file type.
 * - @c open: Finish opening @p file for @p inode.
 * - @c readdir: Emit directory entries through @p filldir.
 * - @c poll: Report readiness and optionally register wait queues.
 * - @c ioctl: Handle file-type-specific ioctl commands.
 * - @c release: Release file-private resources at final close.
 */
struct file_operations {
	ssize_t (*read)(struct file *file, char *buf, size_t count, loff_t pos);
	ssize_t (*write)(struct file *file, const char *buf, size_t count,
			 loff_t pos);
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);
	int (*open)(struct inode *inode, struct file *file);
	int (*readdir)(struct file *file, void *ctx, filldir_t filldir);
	int (*poll)(struct file *file, uint32_t events, struct task_wait *wait);
	int (*ioctl)(struct file *file, uint64_t cmd, uint64_t arg);
	int (*release)(struct file *file);
};

/**
 * @struct file_system_type
 * @brief Registered filesystem driver and mount entry point.
 *
 * @par Fields
 * - @c name: Filesystem type name, for example "ext2".
 * - @c probe: Optional root-autodetect hook. Returns positive for a match,
 *   zero for no match, or a negative errno for hard probe failure.
 * - @c mount: Mount hook. Returns 0 and fills @c out_sb on success, or a
 *   negative errno on failure.
 * - @c next: Intrusive node in the global fs list.
 */
struct file_system_type {
	const char *name;
	int (*probe)(dev_t dev);
	int (*mount)(struct file_system_type *fs_type, dev_t dev,
		     const void *data, struct super_block **out_sb);
	struct file_system_type *next;
};

/**
 * @struct super_block
 * @brief Mounted filesystem instance shared by inodes and mount state.
 *
 * @par Fields
 * - @c s_dev: Device id backing this superblock.
 * - @c s_blocksize: Filesystem block size in bytes.
 * - @c s_flags: Mount/superblock flags.
 * - @c s_root: Root dentry of this filesystem.
 * - @c s_op: Filesystem super operations.
 * - @c s_type: Registered filesystem driver.
 * - @c s_private: Filesystem-private superblock data.
 * - @c s_inodes: Inodes attached to this superblock.
 */
struct super_block {
	dev_t s_dev;
	uint32_t s_blocksize;
	uint32_t s_flags;
	struct dentry *s_root;
	const struct super_operations *s_op;
	struct file_system_type *s_type;
	void *s_private;
	struct list_head s_inodes;
};

/**
 * @struct inode
 * @brief VFS inode metadata and page-cache identity.
 *
 * @par Fields
 * - @c i_ino: Filesystem inode number.
 * - @c i_mode: Linux stat mode bits, including file type.
 * - @c i_uid: Owner uid reported through stat-like syscalls.
 * - @c i_gid: Owner gid reported through stat-like syscalls.
 * - @c i_nlink: Link count.
 * - @c i_size: File size in bytes.
 * - @c i_blocks: Allocated 512-byte blocks for stat ABI.
 * - @c i_atime_sec: Last access time, seconds.
 * - @c i_mtime_sec: Last modification time, seconds.
 * - @c i_ctime_sec: Last metadata-change time, seconds.
 * - @c i_rdev: Device id for special files.
 * - @c i_refcount: Lifetime reference count.
 * - @c i_lock: Serializes inode data mutations (rank 130).
 * - @c i_sb: Owning superblock.
 * - @c i_op: Namespace/metadata operations.
 * - @c i_fop: Default file operations.
 * - @c i_pages: Page-cache mapping for file data.
 * - @c i_private: Filesystem-private inode data.
 * - @c i_hash: Node in inode lookup hash.
 * - @c i_sb_list: Node in superblock inode list.
 */
struct inode {
	uint64_t i_ino;
	uint32_t i_mode;
	uint32_t i_uid;
	uint32_t i_gid;
	uint32_t i_nlink;
	uint64_t i_size;
	uint64_t i_blocks;
	int64_t i_atime_sec;
	int64_t i_mtime_sec;
	int64_t i_ctime_sec;
	dev_t i_rdev;
	refcount_t i_refcount;
	mutex_t i_lock;
	struct super_block *i_sb;
	const struct inode_operations *i_op;
	const struct file_operations *i_fop;
	struct page_mapping i_pages;
	void *i_private;
	struct hlist_node i_hash;
	struct list_head i_sb_list;
};

/**
 * @struct dentry
 * @brief VFS name-cache object binding a path component to an inode.
 *
 * @par Fields
 * - @c d_name: NUL-terminated component name.
 * - @c d_namelen: Component length excluding NUL.
 * - @c d_refcount: Lifetime reference count.
 * - @c d_inode: Resolved inode, or NULL for negative dentry.
 * - @c d_parent: Parent dentry; root points to itself.
 * - @c d_sb: Superblock containing this dentry.
 * - @c d_fsdata: Filesystem-private dentry data.
 * - @c d_hash: Node in dentry lookup hash.
 * - @c d_child: Node in parent's child list.
 * - @c d_subdirs: Head of child dentries.
 */
struct dentry {
	char d_name[VFS_NAME_MAX + 1];
	uint8_t d_namelen;
	refcount_t d_refcount;
	struct inode *d_inode;
	struct dentry *d_parent;
	struct super_block *d_sb;
	void *d_fsdata;
	struct hlist_node d_hash;
	struct list_head d_child;
	struct list_head d_subdirs;
};

/**
 * @struct vfsmount
 * @brief Mounted tree instance and mount-parent relationship.
 *
 * @par Fields
 * - @c mnt_refcount: Long-lived mount reference count.
 * - @c mnt_active_refs: Active path-walk/file operation refs.
 * - @c mnt_list: Node in global mount list.
 * - @c mnt_parent: Parent mount, or self for root.
 * - @c mnt_mountpoint: Dentry covered in parent mount.
 * - @c mnt_root: Root dentry visible through this mount.
 * - @c mnt_sb: Superblock mounted here.
 * - @c mnt_dev: Backing device id.
 * - @c mnt_is_root: True for the root mount.
 */
struct vfsmount {
	refcount_t mnt_refcount;
	atomic_t mnt_active_refs;
	struct list_head mnt_list;
	struct vfsmount *mnt_parent;
	struct dentry *mnt_mountpoint;
	struct dentry *mnt_root;
	struct super_block *mnt_sb;
	dev_t mnt_dev;
	bool mnt_is_root;
};

/**
 * @struct path
 * @brief Pair of mount and dentry that identifies a VFS object.
 *
 * @par Fields
 * - @c mnt: Mount containing @ref dentry.
 * - @c dentry: Dentry within @ref mnt.
 */
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};

/**
 * @struct file
 * @brief Open file description stored in a task fdtable.
 *
 * @par Fields
 * - @c f_op: Operations for this open file.
 * - @c f_path: Path pinned by this open file.
 * - @c f_inode: Cached inode for fast access.
 * - @c private_data: File-type-private open state.
 * - @c f_pos: Current file offset for non-positional I/O.
 * - @c f_flags: Linux O_* status flags.
 * - @c f_mode: FMODE_* access mode bits.
 * - @c refcount: Open-file reference count.
 * - @c static_file: True for non-freeable built-in file objects.
 * - @c f_lock: Serializes @c f_pos for regular files (rank 110).  A mutex,
 *   like Linux's f_pos_lock: the data path allocates page-cache pages,
 *   which the debug-context gate forbids under a spinlock.
 */
struct file {
	const struct file_operations *f_op;
	struct path f_path;
	struct inode *f_inode;
	void *private_data;
	loff_t f_pos;
	uint32_t f_flags;
	uint32_t f_mode;
	refcount_t refcount;
	mutex_t f_lock;
	bool static_file;
};

/**
 * @brief Open a path relative to the current task cwd.
 * @param path Kernel string path.
 * @param flags Linux O_* open flags.
 * @param mode Creation mode before umask handling.
 * @return File descriptor, or a negative errno.
 */
__must_check
int vfs_open(const char *path, uint32_t flags, uint32_t mode);

/**
 * @brief Open a path relative to an explicit base path.
 * @param base Base path for relative lookup.
 * @param path Kernel string path.
 * @param flags Linux O_* open flags.
 * @param mode Creation mode before umask handling.
 * @return File descriptor, or a negative errno.
 */
__must_check
int vfs_openat_path(const struct path *base, const char *path,
				 uint32_t flags, uint32_t mode);

/**
 * @brief Open a path relative to a base dentry.
 * @param base Base dentry for relative lookup.
 * @param path Kernel string path.
 * @param flags Linux O_* open flags.
 * @param mode Creation mode before umask handling.
 * @return File descriptor, or a negative errno.
 */
__must_check
int vfs_openat(struct dentry *base, const char *path, uint32_t flags, uint32_t mode);

__must_check
int file_get_status_flags(struct file *file);

__must_check
int file_set_status_flags(struct file *file, uint32_t flags);

/**
 * @brief Read from an open file at and advancing f_pos.
 * @param file Open file.
 * @param buf Kernel destination buffer.
 * @param count Maximum bytes to read.
 * @return Bytes read, 0 at EOF, or a negative errno.
 */
__must_check
ssize_t vfs_read(struct file *file, char *buf, size_t count);

/**
 * @brief Write to an open file at and advancing f_pos.
 * @param file Open file.
 * @param buf Kernel source buffer.
 * @param count Bytes to write.
 * @return Bytes written, or a negative errno.
 */
__must_check
ssize_t vfs_write(struct file *file, const char *buf, size_t count);

/**
 * @brief Positional read that uses @p pos instead of file->f_pos.
 * @param file Open file.
 * @param buf Kernel destination buffer.
 * @param count Maximum bytes to read.
 * @param pos File offset pointer updated by the operation.
 * @return Bytes read, 0 at EOF, or a negative errno.
 */
__must_check
ssize_t vfs_read_pos(struct file *file, char *buf, size_t count, loff_t *pos);

/**
 * @brief Positional write that uses @p pos instead of file->f_pos.
 * @param file Open file.
 * @param buf Kernel source buffer.
 * @param count Bytes to write.
 * @param pos File offset pointer updated by the operation.
 * @return Bytes written, or a negative errno.
 */
__must_check
ssize_t vfs_write_pos(struct file *file, const char *buf, size_t count, loff_t *pos);

void vfs_rewind_pos(struct file *file, loff_t count);

__must_check
ssize_t vfs_copy_file_buffered(struct file *out_file, struct file *in_file,
		loff_t *in_pos, loff_t *out_pos, size_t len);

/**
 * @brief Reposition an open file according to Linux SEEK_* semantics.
 * @param file Open file.
 * @param offset Offset interpreted by @p whence.
 * @param whence SEEK_SET, SEEK_CUR, SEEK_END, or supported extension.
 * @return New file position, or a negative errno.
 */
__must_check
loff_t vfs_llseek(struct file *file, loff_t offset, int whence);

/**
 * @brief Iterate directory entries for an open directory file.
 * @param file Open directory.
 * @param ctx Caller-owned filldir context.
 * @param filldir Callback receiving Linux dirent-style fields.
 * @return 0 on success, or a negative errno.
 */
__must_check
int vfs_readdir(struct file *file, void *ctx, filldir_t filldir);

__must_check
int vfs_truncate_file(struct file *file, uint64_t size);

__must_check
int vfs_fallocate_file(struct file *file, int mode, uint64_t offset, uint64_t len);

__must_check
int vfs_inode_truncate(struct inode *inode, uint64_t size);

__must_check
int vfs_inode_writeback(struct inode *inode);

__must_check
int vfs_inode_datasync(struct inode *inode);

__must_check
int vfs_inode_set_timestamps(struct inode *inode, int64_t atime_sec, int64_t mtime_sec,
					  bool set_atime, bool set_mtime);

__must_check
int vfs_inode_touch(struct inode *inode, bool atime, bool mtime, bool ctime);

__must_check
int vfs_inode_setattr(struct inode *inode, const struct vfs_inode_attrs *attrs);

__must_check
int vfs_datasync_file(struct file *file);

__must_check
int vfs_sync_file(struct file *file);
/**
 * @brief Convert an inode into Linux stat ABI fields.
 * @param inode Inode to snapshot.
 * @param st Kernel buffer receiving struct stat.
 * @return 0 on success, or a negative errno.
 */
__must_check
int vfs_stat_inode(const struct inode *inode, struct stat *st);

/**
 * @brief Stat an open file through its inode.
 * @param file Open file.
 * @param st Kernel buffer receiving struct stat.
 * @return 0 on success, or a negative errno.
 */
__must_check
int vfs_stat_file(struct file *file, struct stat *st);

__must_check
int vfs_statfs(struct super_block *sb, struct statfs64 *buf);
/**
 * @brief Poll one file for Linux POLL* readiness bits.
 * @param file Open file.
 * @param events Requested POLL* event mask.
 * @param context Optional wait session used to watch readiness channels.
 * @return Ready event mask, or a negative errno.
 */
__must_check
int vfs_poll(struct file *file, uint32_t events, struct task_wait *wait);

__must_check
int vfs_ioctl(struct file *file, uint64_t cmd, uint64_t arg);

__must_check
int vfs_getcwd_path(const struct path *cwd, char *buf,
				 size_t size);

__must_check __pure
static inline uint64_t vfs_inode_size(const struct inode *inode)
{
	return inode ? inode->i_size : 0;
}
__must_check __pure
static inline uint64_t vfs_inode_blocks(const struct inode *inode)
{
	return inode ? inode->i_blocks : 0;
}
__must_check __pure
static inline uint64_t vfs_inode_number(const struct inode *inode)
{
	return inode ? inode->i_ino : 0;
}
__must_check __pure
static inline uint32_t vfs_inode_mode(const struct inode *inode)
{
	return inode ? inode->i_mode : 0;
}
__must_check __pure
static inline dev_t vfs_inode_rdev(const struct inode *inode)
{
	return inode ? inode->i_rdev : 0;
}
__must_check __pure
static inline uint32_t vfs_inode_uid(const struct inode *inode)
{
	return inode ? inode->i_uid : 0;
}
__must_check __pure
static inline uint32_t vfs_inode_gid(const struct inode *inode)
{
	return inode ? inode->i_gid : 0;
}
__must_check __pure
static inline uint32_t vfs_inode_nlink(const struct inode *inode)
{
	return inode ? inode->i_nlink : 0;
}
__must_check __pure
static inline int64_t vfs_inode_atime_sec(const struct inode *inode)
{
	return inode ? inode->i_atime_sec : 0;
}
__must_check __pure
static inline int64_t vfs_inode_mtime_sec(const struct inode *inode)
{
	return inode ? inode->i_mtime_sec : 0;
}
__must_check __pure
static inline int64_t vfs_inode_ctime_sec(const struct inode *inode)
{
	return inode ? inode->i_ctime_sec : 0;
}
__must_check __pure
static inline dev_t vfs_inode_dev(const struct inode *inode)
{
	return inode && inode->i_sb ? inode->i_sb->s_dev : 0;
}
__must_check __pure
static inline struct inode *vfs_dentry_inode(struct dentry *dentry)
{
	return dentry ? dentry->d_inode : NULL;
}
__must_check __pure
static inline struct dentry *vfs_dentry_parent(struct dentry *dentry)
{
	return dentry ? dentry->d_parent : NULL;
}
__must_check __pure
static inline const char *vfs_dentry_name(struct dentry *dentry)
{
	return dentry ? dentry->d_name : NULL;
}
__must_check __pure
static inline size_t vfs_dentry_namelen(struct dentry *dentry)
{
	return dentry ? dentry->d_namelen : 0;
}

#endif

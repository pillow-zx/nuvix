#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/cleanup.h>
#include <nuvix/ctypes.h>
#include <nuvix/hashtable.h>
#include <nuvix/mutex.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>

#define ICACHE_HASH_BITS 7
#define DCACHE_HASH_BITS 10

#define VFS_NAME_MAX     255

#define S_IFMT           0170000
#define S_IFREG          0100000
#define S_IFDIR          0040000
#define S_IFLNK          0120000
#define S_IFCHR          0020000

#define S_ISREG(mode)    (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode)    (((mode) & S_IFMT) == S_IFDIR)
#define S_ISLNK(mode)    (((mode) & S_IFMT) == S_IFLNK)
#define S_ISCHR(mode)    (((mode) & S_IFMT) == S_IFCHR)

#define FMODE_READ       (1U << 0)
#define FMODE_WRITE      (1U << 1)

struct blkdev;
struct page_mapping;
struct super_block;
struct inode;
struct dentry;
struct file;

/*
 * Required methods. alloc_inode returns the backend's containing allocation
 * through its embedded inode, or NULL. VFS initializes the common fields.
 * read_inode fills attributes, operation tables and backend state, returning
 * 0 or a negative errno; failure must leave the object safe for evict_inode.
 * evict_inode releases resources, using i_load_error to distinguish a failed
 * load from a ready inode. destroy_inode only frees the containing allocation
 * and backend allocation state; it also accepts candidates never read/published.
 * All callbacks run outside the icache lock and may sleep, but must not acquire
 * s_lock or recursively look up the inode currently loading/evicting.
 */
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    int (*read_inode)(struct inode *inode);
    void (*evict_inode)(struct inode *inode);
    void (*destroy_inode)(struct inode *inode);
};

struct inode_operations {
    /* namespace held; return 0 with an inode number, or a negative errno. */
    int (*lookup)(struct inode *dir, const char *name, size_t len,
                  uint64_t *ino);
};

struct file_operations {
    /*
     * Optional; 0 or negative errno. Receives an initialized, unpublished file;
     * may replace f_ops with a non-NULL table and initialize private state.
     * Must preserve the VFS-owned references and clean up private resources on
     * failure. The file stays unpublished until file_open returns successfully.
     */
    int (*open)(struct inode *inode, struct file *file);
    /* Optional; called only for a successfully opened file, and may sleep. */
    void (*release)(struct file *file);
};

/* wait.lock protects the index, inode state, lookup refs and final drops. */
struct icache {
    DECLARE_HASHTABLE(entries, ICACHE_HASH_BITS);
    struct wait_queue wait;
};

/* Global name index; namespace may precede lock, never the reverse. */
struct dcache {
    spinlock_t lock;
    DECLARE_HASHTABLE(names, DCACHE_HASH_BITS);
};

/* The fixed instance and its borrowed device/ops outlive every VFS object. */
struct super_block {
    struct blkdev *s_bdev;
    const struct super_operations *s_ops;
    void *s_private;
    struct icache s_icache;
    struct mutex s_lock;
    struct dentry *s_root; /* Owns a reference once the root is installed. */
};

enum inode_state {
    INODE_LOADING,
    INODE_READY,
    INODE_FAILED,
    INODE_FREEING,
};

/*
 * (i_sb, i_ino) is immutable. The index owns no reference and retains a FREEING
 * identity until eviction finishes; no idle inode or zero-count resurrection.
 */
struct inode {
    refcount_t i_count;
    struct super_block *i_sb;
    uint64_t i_ino;
    struct hlist_node i_hash;
    enum inode_state i_state;
    int i_load_error;

    uint32_t i_mode;
    uint32_t i_nlink;
    uint64_t i_size;
    struct mutex i_wlock;
    const struct inode_operations *i_ops;
    const struct file_operations *i_fops;
    /* Owned ordinary-file cache, when present. */
    struct page_mapping *i_mapping;
};

/*
 * Positive entries own an inode reference; all non-root entries own a parent
 * reference. The root points to itself without taking a parent reference.
 * Names are owned buffers with explicit lengths; NULL d_inode is a negative
 * entry. The name index holds no reference and retains no zero-count entries.
 * Namespace protects name changes; the dcache short lock additionally protects
 * hash keys, index membership, lookup references and final reference removal.
 * Final cleanup runs outside the short lock and may sleep through inode eviction.
 */
struct dentry {
    refcount_t d_count;
    struct super_block *d_sb;
    struct dentry *d_parent;
    char *d_name;
    size_t d_name_len;
    uint64_t d_name_hash;
    struct inode *d_inode;
    struct hlist_node d_hash;
};

/* Fixed mount borrows the instance and its root; a path owns its dentry ref. */
struct mount {
    struct super_block *sb;
    struct dentry *root;
};

struct path {
    struct mount *mnt;
    struct dentry *dentry;
};

/* f_path owns the lifetime; f_inode is a borrowed shortcut to its inode. */
struct file {
    refcount_t f_count;
    struct path f_path;
    struct inode *f_inode;
    const struct file_operations *f_ops;
    unsigned int f_mode;
    unsigned int f_flags;
    struct mutex f_pos_lock;
    int64_t f_pos;
    void *f_private;
};

/* Exclusive initialization before publication; does not load/install a root. */
void super_init(struct super_block *sb, struct blkdev *bdev,
                const struct super_operations *ops, void *private);

/*
 * Get one READY reference by instance/inode number, or return ERR_PTR(-errno).
 * Ordinary sleepable thread context; may hold namespace, but no icache, inode
 * wlock or mapping locks. The instance and its required methods must be ready.
 * Concurrent loads share one object; load errors are not retained as idle entries.
 */
struct inode *iget(struct super_block *sb, uint64_t ino);

/* Duplicate a caller-owned READY reference; never acquire through a bare pointer. */
void ihold(struct inode *inode);

/*
 * Drop an owned reference in ordinary sleepable context. The final drop evicts
 * before removing the indexed identity; no icache, inode wlock or mapping locks
 * may be held. Backend eviction must not wait on work requiring caller-held locks.
 */
void iput(struct inode *inode);

DECLARE_FREE(iput, struct inode *, if (_T && !IS_ERR(_T)) iput(_T));

/*
 * Initialize one permanent mount and install the instance root. Exclusive use
 * of initialized sb and unpublished mnt, in ordinary sleepable context. On
 * failure neither root nor mount is installed. Returns 0 or a negative errno.
 */
int mount_init(struct mount *mnt, struct super_block *sb, uint64_t root_ino);

/*
 * Allocate an unhashed negative dentry with one reference, or return NULL.
 * Copies a valid name of len bytes and takes a parent reference when non-NULL;
 * a NULL parent creates a self-parented root. Does not acquire an inode ref.
 */
struct dentry *dentry_alloc(struct super_block *sb, struct dentry *parent,
                            const char *name, size_t len);

/* Duplicate an owned reference; NULL returns NULL. */
struct dentry *dget(struct dentry *dentry);

/*
 * Drop an owned reference; NULL is a no-op. Ordinary sleepable context, with no
 * dcache/icache, inode wlock or mapping locks held. Does not acquire namespace;
 * final cleanup, including parent references, happens outside the dcache lock.
 */
void dput(struct dentry *dentry);

DECLARE_FREE(dput, struct dentry *, if (_T && !IS_ERR(_T)) dput(_T));

/*
 * Remove a non-root name from the index, preserving its inode/parent association
 * and all owned references. Caller owns a reference and holds namespace.
 */
void ddrop(struct dentry *dentry);

/*
 * Caller owns parent and holds its instance namespace throughout. name is a
 * stable kernel buffer of len bytes. Returns an owned positive/negative dentry
 * or ERR_PTR(-errno); only backend lookup ENOENT creates a negative entry.
 * Handles . and .., but performs no path splitting or symlink following.
 * Ordinary sleepable context, with no short, inode wlock or mapping locks held.
 */
struct dentry *dentry_lookup(struct dentry *parent, const char *name, size_t len);

/*
 * Duplicate/drop the dentry reference of an owned path; mount remains borrowed.
 * Neither changes the path fields; path_put has the same context as dput.
 */
void path_get(const struct path *path);
void path_put(const struct path *path);

DECLARE_FREE(path_put, struct path, path_put(&_T));

/*
 * Create an opened file with one reference, or ERR_PTR(-errno). Caller owns a
 * stable path with a non-NULL dentry/mount belonging to the same instance.
 * Takes its own path reference without consuming the caller's. mode consists
 * of FMODE_READ/WRITE; flags are caller-validated file status flags, copied as-is.
 * Path resolution and create/truncate/type checks precede this operation.
 * Ordinary sleepable context, with no caller locks; open/release may sleep.
 */
struct file *file_open(const struct path *path, unsigned int mode,
                       unsigned int flags);

/*
 * Duplicate an owned reference, or acquire under publication protection that
 * guarantees a live, opened file. A bare pointer is insufficient; NULL returns
 * NULL. The caller's publication lock also supplies object visibility.
 */
struct file *fget(struct file *file);

/*
 * Drop an owned reference; NULL is a no-op. Ordinary sleepable context with no
 * caller locks. Final release runs with the path/inode still alive, then drops
 * the path and frees the file. Failed opens never call release.
 */
void fput(struct file *file);

#endif /* _NUVIX_VFS_H */

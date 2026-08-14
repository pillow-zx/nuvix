/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NUVIX_FS_VFS_NAMEI_INTERNAL_H
#define _NUVIX_FS_VFS_NAMEI_INTERNAL_H

#include <nuvix/fs.h>
#include <nuvix/types.h>

__must_check
struct dentry *vfs_lookup_one(struct dentry *parent, const char *name, size_t len);

__must_check
struct dentry *vfs_lookup_one_any(struct dentry *parent, const char *name, size_t len);

void vfs_set_root_dentry(struct dentry *dentry);

#endif

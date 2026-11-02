obj-y 										+= fs/pipe.c
obj-y 										+= fs/vfs/fdtable.c
obj-y 										+= fs/vfs/file.c
obj-y 									  += fs/vfs/kiov.c
obj-y 										+= fs/vfs/super.c
obj-y 										+= fs/vfs/path.c
obj-y 										+= fs/vfs/dentry.c
obj-y 									 	+= fs/vfs/inode.c

obj-$(CONFIG_EXT2_FS) 		+= fs/ext2/file.c
obj-$(CONFIG_EXT2_FS) 		+= fs/ext2/dir.c
obj-$(CONFIG_EXT2_FS) 		+= fs/ext2/inode.c
obj-$(CONFIG_EXT2_FS) 		+= fs/ext2/super.c


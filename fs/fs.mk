obj-y 										+= fs/vfs/fdtable.c
obj-y 										+= fs/vfs/file.c
obj-y 									  += fs/vfs/kiov.c
obj-y 										+= fs/vfs/super.c
obj-y 										+= fs/vfs/path.c
obj-y 										+= fs/vfs/dentry.c
obj-y 									 	+= fs/vfs/inode.c

obj-$(CONFIG_FAT_FS) 			+= fs/fat/bridge.c
obj-$(CONFIG_FAT_FS) 			+= fs/fat/ff.c

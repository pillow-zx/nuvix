# ext2 filesystem.

obj-$(CONFIG_EXT2_FS) += fs/ext2/super.o
obj-$(CONFIG_EXT2_FS) += fs/ext2/inode.o
obj-$(CONFIG_EXT2_FS) += fs/ext2/dir.o
obj-$(CONFIG_EXT2_FS) += fs/ext2/file.o
obj-$(CONFIG_EXT2_FS) += fs/ext2/balloc.o

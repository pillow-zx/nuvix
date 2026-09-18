# Filesystem core and the filesystems built into the image.

obj-y += fs/filesystems.o
obj-y += fs/pipe.o
obj-y += fs/io.o
obj-y += fs/io_uring.o

include $(srctree)/fs/vfs/vfs.mk
include $(srctree)/fs/ext2/ext2.mk

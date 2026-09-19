# Kernel library routines.

obj-y += lib/vsprintf.o
obj-y += lib/rbtree.o
obj-y += lib/sort.o
obj-y += lib/memswap.o
obj-y += lib/memchr.o
obj-y += lib/dt.o

include lib/fdt/fdt.mk

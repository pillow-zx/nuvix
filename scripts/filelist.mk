# Kernel filelist: source manifest.

ARCH_OBJS = \
	arch/riscv/boot.o              \
	arch/riscv/entry.o             \
	arch/riscv/uaccess.o           \
	arch/riscv/uaccess_fixup.o     \
	arch/riscv/switch.o            \
	arch/riscv/trap.o              \
	arch/riscv/task.o              \
	arch/riscv/trap_init.o         \
	arch/riscv/timer.o             \
	arch/riscv/plic.o              \
	arch/riscv/sbi.o               \
	arch/riscv/platform.o          \
	arch/riscv/smp.o               \
	arch/riscv/mm/page_table.o     \
	arch/riscv/mm/user_map.o       \
	arch/riscv/mm/tlb.o            \
	arch/riscv/lib/softfloat.o     \
	arch/riscv/lib/memcpy.o        \
	arch/riscv/lib/memset.o        \
	arch/riscv/lib/memcmp.o        \
	arch/riscv/lib/memmove.o       \
	arch/riscv/lib/strlen.o        \
	arch/riscv/lib/strnlen.o       \
	arch/riscv/lib/strcmp.o        \
	arch/riscv/lib/strncmp.o       \
	arch/riscv/lib/strcpy.o        \
	arch/riscv/lib/strncpy.o       \
	arch/riscv/lib/strchr.o        \
	arch/riscv/lib/strrchr.o

INIT_OBJS = \
	init/main.o        \
	init/bootinfo.o

KERNEL_OBJS = \
	kernel/printk.o         \
	kernel/smp.o            \
	kernel/ipi.o            \
	kernel/stacktrace.o     \
	kernel/mutex.o          \
	kernel/task.o           \
	kernel/proc.o           \
	kernel/fork.o           \
	kernel/futex.o          \
	kernel/rseq.o           \
	kernel/random.o         \
	kernel/reboot.o         \
	kernel/user_return.o    \
	kernel/exec.o           \
	kernel/exit.o           \
	kernel/pid.o            \
	kernel/signal.o         \
	kernel/session.o        \
	kernel/tty.o            \
	kernel/tty_console.o    \
	kernel/waitqueue.o      \
	kernel/time.o           \
	kernel/worker.o         \
	kernel/init_process.o

SCHED_OBJS = \
	sched/sched.o \
	sched/rr.o

MM_OBJS = \
	mm/buddy.o      \
	mm/slab.o       \
	mm/vmalloc.o    \
	mm/user_map.o   \
	mm/vma.o        \
	mm/mmap.o       \
	mm/mmap_flush.o \
	mm/page_fault.o \
	mm/uaccess.o

VFS_OBJS = \
	fs/vfs/super.o          \
	fs/vfs/inode.o          \
	fs/vfs/dentry.o         \
	fs/vfs/mount.o          \
	fs/vfs/namei.o          \
	fs/vfs/namei_mutation.o \
	fs/vfs/fs_struct.o      \
	fs/vfs/fdtable.o        \
	fs/vfs/file.o           \
	fs/vfs/eventpoll.o      \
	fs/vfs/read_write.o

EXT2_OBJS = \
	fs/ext2/super.o \
	fs/ext2/inode.o \
	fs/ext2/dir.o   \
	fs/ext2/file.o  \
	fs/ext2/balloc.o

FS_OBJS = \
	fs/filesystems.o \
	fs/pipe.o        \
	$(VFS_OBJS)

BLOCK_OBJS = \
	block/blkdev.o               \
	block/page_cache_alias.o     \
	block/page_cache.o           \
	block/page_cache_dirty.o     \
	block/page_cache_writeback.o \
	block/virtio_blk.o

DRIVER_OBJS = \
	drivers/uart.o

SYSCALL_OBJS = \
	syscall/syscall.o          \
	syscall/sys_proc.o         \
	syscall/sys_task.o         \
	syscall/sys_file_helpers.o \
	syscall/sys_file_io.o      \
	syscall/sys_file_path.o    \
	syscall/sys_file_stat.o    \
	syscall/sys_file_poll.o    \
	syscall/sys_exec.o         \
	syscall/sys_mm.o           \
	syscall/sys_signal.o       \
	syscall/sys_futex.o        \
	syscall/sys_log.o          \
	syscall/sys_membarrier.o   \
	syscall/sys_misc.o         \
	syscall/sys_sched.o        \
	syscall/sys_rseq.o         \
	syscall/sys_time.o

LIB_OBJS = \
	lib/vsprintf.o \
	lib/rbtree.o   \
	lib/sort.o

OBJ-y = \
	$(ARCH_OBJS)    \
	$(INIT_OBJS)    \
	$(KERNEL_OBJS)  \
	$(MM_OBJS)      \
	$(FS_OBJS)      \
	$(BLOCK_OBJS)   \
	$(DRIVER_OBJS)  \
	$(SCHED_OBJS)   \
	$(SYSCALL_OBJS) \
	$(LIB_OBJS)

OBJ-$(CONFIG_EXT2_FS) += $(EXT2_OBJS)

# Keep disabled-option artifacts in the clean set after a configuration flip.
ALL_OBJ_REL = $(sort $(OBJ-y) $(EXT2_OBJS))
OBJ_REL = $(OBJ-y)

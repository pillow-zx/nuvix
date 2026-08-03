# Kernel filelist: source manifest.

ARCH_OBJS = \
	arch/riscv/boot.o              \
	arch/riscv/entry.o             \
	arch/riscv/switch.o            \
	arch/riscv/trap.o              \
	arch/riscv/task.o              \
	arch/riscv/trap_init.o         \
	arch/riscv/timer.o             \
	arch/riscv/plic.o              \
	arch/riscv/sbi.o               \
	arch/riscv/mm/page_table.o     \
	arch/riscv/mm/user_map.o       \
	arch/riscv/mm/tlb.o            \
	arch/riscv/lib/softfloat.o     \
	arch/riscv/lib/string.o

INIT_OBJS = \
	init/main.o

KERNEL_OBJS = \
	kernel/printk.o         \
	kernel/ksyms.o          \
	kernel/stacktrace.o     \
	kernel/mutex.o          \
	kernel/task.o           \
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
	sched/mlfq.o

MM_OBJS = \
	mm/buddy.o      \
	mm/slab.o       \
	mm/vmalloc.o    \
	mm/user_map.o   \
	mm/vma.o        \
	mm/mmap.o       \
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

ifeq ($(CONFIG_EXT2_FS),y)
FS_EXT2_OBJS = $(EXT2_OBJS)
else
FS_EXT2_OBJS =
endif

FS_OBJS = \
	fs/filesystems.o \
	fs/pipe.o        \
	$(VFS_OBJS)      \
	$(FS_EXT2_OBJS)

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
	lib/vsprintf.o

KERNEL_SELFTEST ?= 0
KERNEL_PANIC ?= 0
KERNEL_PANIC_CASE ?=

ifeq ($(KERNEL_SELFTEST),1)
CFLAGS += -DKERNEL_SELFTEST
ASFLAGS += -DKERNEL_SELFTEST
TEST_SRCS = $(shell find test -type f \( -name '*.c' -o -name '*.S' \) | sort)
TEST_OBJS = $(patsubst %.c,%.o,$(patsubst %.S,%.o,$(TEST_SRCS)))
KERNEL_TEST_OBJS = $(TEST_OBJS)
else
KERNEL_TEST_OBJS =
endif

ifeq ($(KERNEL_PANIC),1)
CFLAGS += -DKERNEL_PANIC_TEST
# Panic cases intentionally exercise debug-only held-lock diagnostics even
# when the caller's regular .config has disabled the diagnostic footprint.
CFLAGS += -DCONFIG_DEBUG_CONTEXT=1
KERNEL_PANIC_OBJS = test/kpanic.o
ifeq ($(KERNEL_PANIC_CASE),preempt-underflow)
CFLAGS += -DKPANIC_CASE_PREEMPT_UNDERFLOW
else ifeq ($(KERNEL_PANIC_CASE),preempt-overflow)
CFLAGS += -DKPANIC_CASE_PREEMPT_OVERFLOW
else ifeq ($(KERNEL_PANIC_CASE),spinlock-wrong-unlock)
CFLAGS += -DKPANIC_CASE_SPINLOCK_WRONG_UNLOCK
else ifeq ($(KERNEL_PANIC_CASE),spinlock-recursive)
CFLAGS += -DKPANIC_CASE_SPINLOCK_RECURSIVE
else ifeq ($(KERNEL_PANIC_CASE),spinlock-capacity)
CFLAGS += -DKPANIC_CASE_SPINLOCK_CAPACITY
else ifeq ($(KERNEL_PANIC_CASE),schedule-held-lock)
CFLAGS += -DKPANIC_CASE_SCHEDULE_HELD_LOCK
else ifeq ($(KERNEL_PANIC_CASE),schedule-preempt-disabled)
CFLAGS += -DKPANIC_CASE_SCHEDULE_PREEMPT_DISABLED
else ifeq ($(KERNEL_PANIC_CASE),wait-held-lock)
CFLAGS += -DKPANIC_CASE_WAIT_HELD_LOCK
else ifeq ($(KERNEL_PANIC_CASE),wait-preempt-disabled)
CFLAGS += -DKPANIC_CASE_WAIT_PREEMPT_DISABLED
else ifeq ($(KERNEL_PANIC_CASE),wait-hard-irq)
CFLAGS += -DKPANIC_CASE_WAIT_HARD_IRQ
endif
else
KERNEL_PANIC_OBJS =
endif

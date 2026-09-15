# System call entry and handlers.

obj-y += syscall/syscall.o
obj-y += syscall/sys_proc.o
obj-y += syscall/sys_task.o
obj-y += syscall/sys_file_helpers.o
obj-y += syscall/sys_file_io.o
obj-y += syscall/sys_file_path.o
obj-y += syscall/sys_file_stat.o
obj-y += syscall/sys_file_poll.o
obj-y += syscall/sys_exec.o
obj-y += syscall/sys_mm.o
obj-y += syscall/sys_signal.o
obj-y += syscall/sys_log.o
obj-y += syscall/sys_misc.o
obj-y += syscall/sys_sched.o
obj-y += syscall/sys_time.o

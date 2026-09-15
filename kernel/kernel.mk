# Core kernel services: printing, SMP, synchronisation, tasks, and processes.

obj-y += kernel/printk.o
obj-y += kernel/smp.o
obj-y += kernel/ipi.o
obj-y += kernel/stacktrace.o
obj-y += kernel/mutex.o
obj-y += kernel/rwlock.o
obj-y += kernel/cpu.o
obj-y += kernel/task.o
obj-y += kernel/proc.o
obj-y += kernel/fork.o
obj-y += kernel/random.o
obj-y += kernel/reboot.o
obj-y += kernel/user_return.o
obj-y += kernel/exec.o
obj-y += kernel/exit.o
obj-y += kernel/pid.o
obj-y += kernel/signal.o
obj-y += kernel/session.o
obj-y += kernel/tty.o
obj-y += kernel/tty_console.o
obj-y += kernel/waitqueue.o
obj-y += kernel/time.o
obj-y += kernel/worker.o
obj-y += kernel/init_process.o

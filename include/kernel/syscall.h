#ifndef _CUTEOS_KERNEL_SYSCALL_H
#define _CUTEOS_KERNEL_SYSCALL_H

/**
 * @file syscall.h
 * @brief Syscall handler declarations and dispatch API.
 */

#include <kernel/trap.h>
#include <kernel/syscall_table.h>
#include <kernel/types.h>
#include <uapi/mman.h>
#include <uapi/sched.h>

struct task_struct;

/**
 * @def DECLARE_SYSCALL
 * @brief Expand syscall-table entries into handler declarations.
 */
#define DECLARE_SYSCALL(nr, name, fn) ssize_t fn(struct trap_frame *);
SYSCALL_TABLE(DECLARE_SYSCALL)
#undef DECLARE_SYSCALL

/**
 * @brief Dispatch one Linux riscv64 syscall from a trap frame.
 * @param tf Trap frame containing a7 syscall number and a0-a5 arguments.
 *
 * The dispatcher writes the handler result back to a0. Handlers return
 * non-negative success values or negative Linux errno values.
 */
void do_syscall(struct trap_frame *tf);

/**
 * @brief Initialize the syscall dispatch table from SYSCALL_TABLE.
 */
void syscall_init(void);

#endif

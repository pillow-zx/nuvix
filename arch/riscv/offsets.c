#include <nuvix/assert.h>
#include <nuvix/task.h>
#include <asm/riscv/smp.h>
#include <asm/riscv/trap.h>

#define EMIT_VALUE(name, value) __asm__ volatile("\n.ascii \"->" #name " %c0\"" : : "i" (value))
#define EMIT_OFFSET(name, type, member) EMIT_VALUE(name, offsetof(type, member))

static_assert(sizeof(struct trap_frame) % 16 == 0,
              "trap frame must preserve RISC-V stack alignment");

static_assert(offsetof(struct trap_frame, regs) == 0,
              "trap frame must begin with the zero register slot");

static __used void emit_asm_offsets(void)
{
    EMIT_OFFSET(TASK_CONTEXT_RA_OFFSET, struct task, ctx.ra);
    EMIT_OFFSET(TASK_CONTEXT_SP_OFFSET, struct task, ctx.sp);
    EMIT_OFFSET(TASK_CONTEXT_SREGS_OFFSET, struct task, ctx.s);
    EMIT_VALUE(TASK_CONTEXT_SREGS_STRIDE, sizeof(((struct task_context *)0)->s[0]));
    EMIT_OFFSET(TASK_CONTEXT_FREGS_OFFSET, struct task, ctx.fregs);
    EMIT_VALUE(TASK_CONTEXT_FREGS_STRIDE, sizeof(((struct task_context *)0)->fregs[0]));
    EMIT_OFFSET(TASK_CONTEXT_FCSR_OFFSET, struct task, ctx.fcsr);
    EMIT_OFFSET(TASK_CONTEXT_FS_OFFSET, struct task, ctx.fs);
    EMIT_OFFSET(TASK_CONTEXT_SUM_OFFSET, struct task, ctx.sum);

    EMIT_OFFSET(TRAP_FRAME_REGS_OFFSET, struct trap_frame, regs);
    EMIT_VALUE(TRAP_FRAME_REGS_STRIDE, sizeof(((struct trap_frame *)0)->regs[0]));
    EMIT_OFFSET(TRAP_FRAME_SEPC_OFFSET, struct trap_frame, sepc);
    EMIT_OFFSET(TRAP_FRAME_SSTATUS_OFFSET, struct trap_frame, sstatus);
    EMIT_OFFSET(TRAP_FRAME_SCAUSE_OFFSET, struct trap_frame, scause);
    EMIT_OFFSET(TRAP_FRAME_STVAL_OFFSET, struct trap_frame, stval);
    EMIT_OFFSET(TRAP_FRAME_SATP_OFFSET, struct trap_frame, satp);
    EMIT_OFFSET(TRAP_FRAME_TASK_OFFSET, struct trap_frame, task);
    EMIT_OFFSET(TRAP_FRAME_FREGS_OFFSET, struct trap_frame, fregs);
    EMIT_VALUE(TRAP_FRAME_FREGS_STRIDE, sizeof(((struct trap_frame *)0)->fregs[0]));
    EMIT_OFFSET(TRAP_FRAME_FCSR_OFFSET, struct trap_frame, fcsr);
    EMIT_VALUE(TRAP_FRAME_SIZE, sizeof(struct trap_frame));

    EMIT_OFFSET(CPU_BOOT_DATA_TASK_OFFSET, struct cpu_boot_data, task);
    EMIT_OFFSET(CPU_BOOT_DATA_STACK_OFFSET, struct cpu_boot_data, stack);
    EMIT_OFFSET(CPU_BOOT_DATA_SATP_OFFSET, struct cpu_boot_data, satp);
}

#include <nuvix/task.h>
#include <nuvix/sched.h>
#include <nuvix/layout.h>
#include <nuvix/string.h>
#include <nuvix/errno.h>
#include <asm/trap.h>

void arch_ktask_init(struct task_context *ctx, int (*fn)(void *), void *arg)
{
    ctx->ra = (unsigned long)__ktask_entry;
    ctx->s[0] = (unsigned long)fn;
    ctx->s[1] = (unsigned long)arg;
}

int arch_utask_init(struct task *task, vaddr_t entry, vaddr_t sp)
{
    IFNDEF(CONFIG_FPU, return -ENOEXEC;)

    struct trap_frame *frame =
        (void *)((vaddr_t)task->kstack + KSTACK_SIZE - TRAP_SIZE);

    memset(frame, 0, sizeof(*frame));
    frame->regs[2] = sp;
    frame->sepc = entry;
    frame->sstatus = SSTATUS_SPIE | SSTATUS_UXL_64 | SSTATUS_FS_INITIAL;
    frame->task = task;
    task->ctx.sp = (vaddr_t)frame;
    task->ctx.ra = (unsigned long)__utask_entry;
    return 0;
}

void utask_entry(struct task *prev)
{
    sched_finish_switch(prev);
    preempt_enable_no_resched();
}

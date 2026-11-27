#include <nuvix/task.h>
#include <nuvix/sched.h>
#include <nuvix/page.h>
#include <nuvix/layout.h>
#include <nuvix/string.h>
#include <nuvix/pgtable.h>
#include <nuvix/errno.h>
#include <asm/riscv/trap.h>
#include <asm/riscv/signal.h>

extern const char __sigret_start[], __sigret_end[];

void arch_ktask_init(struct task_context *ctx, int (*fn)(void *), void *arg)
{
    ctx->ra = (unsigned long)__ktask_entry;
    ctx->s[0] = (unsigned long)fn;
    ctx->s[1] = (unsigned long)arg;
}

static inline struct trap_frame *task_uframe(const struct task *task)
{
    return (struct trap_frame *)((vaddr_t)task->kstack + KSTACK_SIZE) - 1;
}

int arch_utask_init(struct task *task, struct mm_struct *mm, vaddr_t entry,
                    vaddr_t sp)
{
    struct trap_frame *frame;
    struct page *page __free(page) = NULL;
    void *code;
    int ret;

    page = alloc_page();
    if (!page)
        return -ENOMEM;

    code = (void *)__va(page_to_phys(page));
    memset(code, 0, PAGE_SIZE);
    memcpy(code, __sigret_start, __sigret_end - __sigret_start);

    mutex_lock(&mm->mmap_lock);
    ret = pgtable_map_page(mm, USER_TRAMPOLINE, page, VM_READ | VM_EXEC);
    mutex_unlock(&mm->mmap_lock);
    if (ret)
        return ret;
    keep(page);

    frame = task_uframe(task);
    memset(frame, 0, sizeof(*frame));
    frame->regs[2] = sp;
    frame->sepc = entry;
    frame->sstatus = SSTATUS_SPIE | SSTATUS_UXL_64;
    frame->sstatus |= SSTATUS_FS_INITIAL;
    frame->task = task;

    task->ctx.sp = (vaddr_t)frame;
    task->ctx.ra = (unsigned long)__utask_entry;
    return 0;
}

void utask_entry(struct task *prev, struct trap_frame *frame)
{
    sched_finish_switch(prev);
    preempt_enable_no_resched();
    local_irq_enable();
    arch_do_signal(frame, NULL);
    local_irq_disable();
}

void arch_task_clone(struct task *task)
{
    const struct trap_frame *frame = task_uframe(current);
    struct trap_frame *child = task_uframe(task);

    *child = *frame;
    child->task = task;
    child->regs[10] = 0;
    if (syscallarg(frame, 1))
        child->regs[2] = syscallarg(frame, 1);
    if (syscallarg(frame, 0) & CLONE_SETTLS)
        child->regs[4] = syscallarg(frame, 3);
    task->ctx.sp = (vaddr_t)child;
    task->ctx.ra = (unsigned long)__utask_entry;
}

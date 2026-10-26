#include <nuvix/task.h>
#include <nuvix/sched.h>
#include <nuvix/page.h>
#include <nuvix/layout.h>
#include <nuvix/string.h>
#include <nuvix/pgtable.h>
#include <nuvix/errno.h>
#include <asm/trap.h>
#include <asm/signal.h>

extern const char __sigret_start[], __sigret_end[];

void arch_ktask_init(struct task_context *ctx, int (*fn)(void *), void *arg)
{
    ctx->ra = (unsigned long)__ktask_entry;
    ctx->s[0] = (unsigned long)fn;
    ctx->s[1] = (unsigned long)arg;
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

    ret = pgtable_map_page(mm, USER_TRAMPOLINE, page,
                           VM_READ | VM_WRITE | VM_EXEC);
    if (ret)
        return ret;
    keep(page);

    frame = (void *)((vaddr_t)task->kstack + KSTACK_SIZE - TRAP_SIZE);
    memset(frame, 0, sizeof(*frame));
    frame->regs[2] = sp;
    frame->sepc = entry;
    frame->sstatus = SSTATUS_SPIE | SSTATUS_UXL_64;
#ifdef CONFIG_FPU
    frame->sstatus |= SSTATUS_FS_INITIAL;
#endif
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

void arch_task_clone(struct task *task, const struct trap_frame *frame,
                    unsigned long sp)
{
    struct trap_frame *child =
        (void *)((vaddr_t)task->kstack + KSTACK_SIZE - TRAP_SIZE);

    *child = *frame;
    child->task = task;
    child->regs[10] = 0;
    if (sp)
        child->regs[2] = sp;
    task->ctx.sp = (vaddr_t)child;
    task->ctx.ra = (unsigned long)__utask_entry;
}

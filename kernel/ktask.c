#include <nuvix/task.h>
#include <nuvix/module.h>
#include <nuvix/errno.h>
#include <nuvix/vfs.h>
#include <nuvix/sched.h>
#include <nuvix/assert.h>
#include <nuvix/execve.h>
#include <nuvix/tty.h>

static __noreturn void ktask_exit(void)
{
    Assert(!current->parent, "kernel thread exit requires a kernel task\n");
    task_exit(0);
}

__noreturn void ktask_entry(struct task *prev, int (*fn)(void *), void *arg)
{
    sched_finish_switch(prev);
    preempt_enable_no_resched();
    local_irq_enable();

    (void)fn(arg);
    ktask_exit();
}

static int kernel_init(void *args)
{
    (void)args;
    const char *const argv[] = {"sbin/init", NULL};
    const char *const envp[] = {NULL};
    int ret;

    ret = tty_init();
    if (ret)
        panic("TTY initialization failed (%d)\n", ret);

    ret = vfs_mount();
    if (ret)
        panic("filesystem mount failed (%d)\n", ret);

    ret = init_execve("/sbin/init", argv, envp);
    if (ret)
        panic("sbin/init load failed (%d)\n", ret);
    return 0;
}

static int ktask_init(void)
{
    struct task *kinit __free(task_put) = NULL;

    kinit = task_alloc(false);
    if (!kinit)
        return -ENOMEM;
    arch_ktask_init(&kinit->ctx, kernel_init, NULL);
    sched_publish(kinit);

    return 0;
}
/* The task can run on another CPU as soon as it is published. */
MODULE_INIT(ktask_init, 999);

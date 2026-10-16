#include <nuvix/kthread.h>
#include <nuvix/errno.h>
#include <nuvix/vfs.h>
#include <nuvix/macro.h>
#include <nuvix/sched.h>
#include <nuvix/assert.h>

static int kernel_init(void *args)
{
    (void)args;
    int ret;

    ret = vfs_mount();
    if (ret)
        panic("FAT mount failed (%d)\n", ret);
    return 0;
}

int kthread_init(void)
{
    struct task *task __free(task_put) = create_kthread(kernel_init, NULL);
    if (!task)
        return -ENOMEM;
    sched_publish(task);

    return task_reaper_init();
}

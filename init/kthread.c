#include <nuvix/kthread.h>
#include <nuvix/sched.h>

int kthread_init(void)
{
    return task_reaper_init();
}

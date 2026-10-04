#include <nuvix/pid.h>
#include <nuvix/bitmap.h>
#include <nuvix/spinlock.h>
#include <nuvix/errno.h>

static DECLARE_BITMAP(pidmap, PID_MAX) = {BIT_MASK(0)};
static unsigned int next_pid = 1;
static DEFINE_SPINLOCK(pid_lock);

int pid_alloc(void)
{
    unsigned long pid;

    spin_lock(&pid_lock);
    pid = find_next_zero_bit(pidmap, PID_MAX, next_pid);
    if (pid == PID_MAX) {
        pid = find_next_zero_bit(pidmap, next_pid, 1);
        if (pid == next_pid) {
            spin_unlock(&pid_lock);
            return -EAGAIN;
        }
    }
    set_bit(pid, pidmap);
    next_pid = pid + 1;
    if (next_pid == PID_MAX)
        next_pid = 1;
    spin_unlock(&pid_lock);
    return (int)pid;
}

void pid_free(unsigned int pid)
{
    spin_lock(&pid_lock);
    clear_bit(pid, pidmap);
    spin_unlock(&pid_lock);
}

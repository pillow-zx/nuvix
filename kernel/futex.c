#include <nuvix/futex.h>
#include <nuvix/assert.h>
#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/list.h>
#include <nuvix/spinlock.h>
#include <nuvix/hashtable.h>
#include <nuvix/compiler.h>
#include <nuvix/uaccess.h>
#include <nuvix/sched.h>
#include <nuvix/preempt.h>
#include <nuvix/pid.h>
#include <nuvix/timer.h>
#include <nuvix/tick.h>
#include <nuvix/math.h>
#include <asm/trap.h>

struct futex_bucket {
    spinlock_t lock;
    struct list_head waiters;
};

struct futex_timeout {
    struct timer timer;
    struct futex_waiter *waiter;
};

static_assert(
    IS_POWER_OF_2(CONFIG_NR_FUTEX_BUCKETS),
    "the number of futex hash buckets must be a positive power of two");

static struct futex_bucket buckets[CONFIG_NR_FUTEX_BUCKETS];

static inline uint64_t buckets_key(const struct futex_key *key)
{
    return ((uint64_t)(uintptr_t)key->mm ^ (uint64_t)(uintptr_t)key->addr);
}

static inline struct futex_bucket *get_bucket(const struct futex_key *key)
{
    return &buckets[hash_64(buckets_key(key), HASH_BITS(buckets))];
}

static unsigned long lock_buckets(struct futex_bucket *a,
                                  struct futex_bucket *b)
{
    struct futex_bucket *first = a < b ? a : b;
    struct futex_bucket *second = a < b ? b : a;
    unsigned long flags;

    spin_lock_irqsave(&first->lock, flags);
    if (first != second)
        spin_lock(&second->lock);
    return flags;
}

static void unlock_buckets(struct futex_bucket *a, struct futex_bucket *b,
                           unsigned long flags)
{
    struct futex_bucket *first = a < b ? a : b;
    struct futex_bucket *second = a < b ? b : a;

    if (first != second)
        spin_unlock(&second->lock);
    spin_unlock_irqrestore(&first->lock, flags);
}

static struct futex_bucket *lock_waiter(struct futex_waiter *waiter,
                                        unsigned long *flags)
{
    for (;;) {
        struct futex_key key = {
            .mm = waiter->key.mm,
            .addr = READ_ONCE(waiter->key.addr),
        };
        struct futex_bucket *bucket = get_bucket(&key);

        spin_lock_irqsave(&bucket->lock, *flags);
        if (key.addr == READ_ONCE(waiter->key.addr))
            return bucket;
        spin_unlock_irqrestore(&bucket->lock, *flags);
    }
}

void futex_init(void)
{
    for (int i = 0; i < CONFIG_NR_FUTEX_BUCKETS; ++i) {
        spin_lock_init(&buckets[i].lock);
        INIT_LIST_HEAD(&buckets[i].waiters);
    }
}

static void futex_timeout_wake(struct timer *timer)
{
    struct futex_timeout *timeout =
        container_of(timer, struct futex_timeout, timer);
    struct futex_waiter *waiter = timeout->waiter;
    struct futex_bucket *bucket;
    unsigned long flags;

    bucket = lock_waiter(waiter, &flags);
    if (!list_empty(&waiter->node))
        (void)sched_wake(waiter->task);
    spin_unlock_irqrestore(&bucket->lock, flags);
}

static int futex_wait(uint32_t *addr, uint32_t val, const uint64_t *deadline)
{
    struct futex_waiter waiter;
    struct futex_timeout timeout;
    struct futex_bucket *bucket, *source;
    struct rq *rq;
    unsigned long flags, rq_flags;
    uint32_t word;
    bool woken;
    int ret;

    might_sleep();
    waiter.key.mm = current->mm;
    waiter.key.addr = addr;
    waiter.task = current;
    waiter.woken = false;
    INIT_LIST_HEAD(&waiter.node);
    source = bucket = get_bucket(&waiter.key);
    if (deadline) {
        timeout.waiter = &waiter;
        timer_init(&timeout.timer, current_cpu_id(), futex_timeout_wake);
        ret = timer_start(&timeout.timer, *deadline);
        if (ret)
            return ret;
    }

    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&bucket->lock, flags);
        if (copy_from_user(&word, addr, sizeof(word))) {
            spin_unlock_irqrestore(&bucket->lock, flags);
            preempt_enable();
            if (copy_from_user(&word, addr, sizeof(word))) {
                ret = -EFAULT;
                goto out;
            }
            continue;
        }
        if (word != val) {
            ret = -EAGAIN;
            break;
        }
        if (deadline && time_read() >= *deadline) {
            ret = -ETIMEDOUT;
            break;
        }
        if (signal_pending(current)) {
            ret = -EINTR;
            break;
        }
        list_add_tail(&waiter.node, &bucket->waiters);
        rq = this_rq();
        spin_lock_irqsave(&rq->lock, rq_flags);
        sched_set_sleep_locked(rq, TASK_INTERRUPTIBLE);
        spin_unlock_irqrestore(&rq->lock, rq_flags);
        spin_unlock_irqrestore(&bucket->lock, flags);
        sched_park();

        bucket = lock_waiter(&waiter, &flags);
        list_del_init(&waiter.node);
        woken = waiter.woken;
        spin_unlock_irqrestore(&bucket->lock, flags);
        if (woken) {
            ret = 0;
            goto out;
        }
        if (deadline && time_read() >= *deadline) {
            ret = -ETIMEDOUT;
            goto out;
        }
        if (signal_pending(current)) {
            ret = -EINTR;
            goto out;
        }
        flags = lock_buckets(bucket, source);
        WRITE_ONCE(waiter.key.addr, addr);
        unlock_buckets(bucket, source, flags);
        bucket = source;
    }
    spin_unlock_irqrestore(&bucket->lock, flags);
    preempt_enable();
out:
    if (deadline)
        timer_cancel_sync(&timeout.timer);
    return ret;
}

int futex_wake(uint32_t *addr, unsigned int nr_wake)
{
    struct futex_key key = {.mm = current->mm, .addr = addr};
    struct futex_bucket *bucket = get_bucket(&key);
    struct futex_waiter *waiter, *next;
    unsigned long flags;
    unsigned int woken = 0;

    if (!nr_wake)
        return 0;
    spin_lock_irqsave(&bucket->lock, flags);
    list_for_each_entry_safe (waiter, next, &bucket->waiters, node) {
        if (waiter->key.mm != key.mm || waiter->key.addr != key.addr)
            continue;
        list_del_init(&waiter->node);
        waiter->woken = true;
        (void)sched_wake(waiter->task);
        if (++woken == nr_wake)
            break;
    }
    spin_unlock_irqrestore(&bucket->lock, flags);
    return woken;
}

static int futex_cmpxchg_u32(uint32_t *observed, uint32_t *addr,
                             uint32_t expected, uint32_t desired)
{
    if ((uintptr_t)addr & (sizeof(*addr) - 1))
        return -EINVAL;
    if (!access_ok(addr, sizeof(*addr)) || !current->mm)
        return -EFAULT;

    return __futex_cmpxchg_u32(observed, addr, expected, desired);
}

static int futex_death(uint32_t *addr, bool pending)
{
    uint32_t tid = task_pid_nr(current, PIDTYPE_TID);
    uint32_t word, observed, desired;
    int ret;

    if ((uintptr_t)addr & (sizeof(*addr) - 1))
        return -EINVAL;

    for (;;) {
        if (copy_from_user(&word, addr, sizeof(word)))
            return -EFAULT;
        if (pending && !(word & FUTEX_TID_MASK)) {
            (void)futex_wake(addr, 1);
            return 0;
        }
        if ((word & FUTEX_TID_MASK) != tid)
            return 0;

        desired = (word & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
        ret = futex_cmpxchg_u32(&observed, addr, word, desired);
        if (ret)
            return ret;
        if (observed == word)
            break;
    }

    if (word & FUTEX_WAITERS)
        (void)futex_wake(addr, 1);
    return 0;
}

void futex_release(void)
{
    struct robust_list_head *head = move(current->robust);
    struct robust_list_head list;
    struct robust_list *entry, *next, *pending;

    if (!head)
        return;
    if (copy_from_user(&list, head, sizeof(list)))
        return;

    entry = list.list.next;
    pending = (void *)((uintptr_t)list.pending & ~1UL);
    for (unsigned int i = 0; i < ROBUST_LIST_LIMIT; ++i) {
        bool pi = (uintptr_t)entry & 1UL;

        entry = (void *)((uintptr_t)entry & ~1UL);
        if (entry == &head->list)
            break;
        if (copy_from_user(&next, entry, sizeof(next)))
            return;
        if (entry != pending && !pi) {
            uint32_t *addr = (void *)((uintptr_t)entry + list.offset);

            if (futex_death(addr, false))
                return;
        }
        entry = next;
    }

    if (pending && !((uintptr_t)list.pending & 1UL)) {
        uint32_t *addr = (void *)((uintptr_t)pending + list.offset);

        (void)futex_death(addr, true);
    }
}

static int futex_requeue(uint32_t *addr, uint32_t *addr2, unsigned int nr_wake,
                         unsigned int nr_requeue)
{
    struct futex_key key = {.mm = current->mm, .addr = addr};
    struct futex_key key2 = {.mm = current->mm, .addr = addr2};
    struct futex_bucket *source = get_bucket(&key);
    struct futex_bucket *target = get_bucket(&key2);
    struct futex_waiter *waiter, *next;
    unsigned int woken = 0, requeued = 0;
    unsigned long flags;

    flags = lock_buckets(source, target);
    list_for_each_entry_safe (waiter, next, &source->waiters, node) {
        if (waiter->key.mm != key.mm || waiter->key.addr != key.addr)
            continue;
        if (woken < nr_wake) {
            list_del_init(&waiter->node);
            waiter->woken = true;
            (void)sched_wake(waiter->task);
            ++woken;
        } else if (requeued < nr_requeue) {
            if (source != target)
                list_move_tail(&waiter->node, &target->waiters);
            WRITE_ONCE(waiter->key.addr, addr2);
            ++requeued;
        } else {
            break;
        }
    }
    unlock_buckets(source, target, flags);
    return woken + requeued;
}

long sys_futex(struct trap_frame *frame)
{
    uint32_t *uaddr = (uint32_t *)syscallarg(frame, 0);
    int op = syscallarg(frame, 1);
    unsigned int val = syscallarg(frame, 2);
    int cmd = op & FUTEX_CMD_MASK;

    if (op & FUTEX_CLOCK_REALTIME)
        return -ENOSYS;
    if ((uintptr_t)uaddr & (sizeof(*uaddr) - 1))
        return -EINVAL;
    if (!access_ok(uaddr, sizeof(*uaddr)))
        return -EFAULT;

    switch (cmd) {
    case FUTEX_WAIT: {
        const struct timespec64 *utime =
            (const struct timespec64 *)syscallarg(frame, 3);
        struct timespec64 ts;
        uint64_t ticks, deadline;
        int ret;

        if (utime) {
            if (copy_from_user(&ts, utime, sizeof(ts)))
                return -EFAULT;
            ret = time_ticks_from_timespec(&ts, &ticks);
            if (ret == -EOVERFLOW)
                ticks = UINT64_MAX;
            else if (ret)
                return ret;
            if (check_add_overflow(time_read(), ticks, &deadline))
                deadline = UINT64_MAX;
        }
        return futex_wait(uaddr, val, utime ? &deadline : NULL);
    }
    case FUTEX_WAKE:
        return futex_wake(uaddr, val);
    case FUTEX_REQUEUE: {
        int nr_requeue = (int)syscallarg(frame, 3);
        uint32_t *uaddr2 = (uint32_t *)syscallarg(frame, 4);

        if ((int)val < 0 || nr_requeue < 0)
            return -EINVAL;
        if ((uintptr_t)uaddr2 & (sizeof(*uaddr2) - 1))
            return -EINVAL;
        if (!access_ok(uaddr2, sizeof(*uaddr2)))
            return -EFAULT;
        return futex_requeue(uaddr, uaddr2, val, nr_requeue);
    }
    default:
        return -ENOSYS;
    }
}

long sys_get_robust_list(struct trap_frame *frame)
{
    int tid = syscallarg(frame, 0);
    struct robust_list_head **uhead =
        (struct robust_list_head **)syscallarg(frame, 1);
    size_t *ulen = (size_t *)syscallarg(frame, 2);

    struct task *task __free(task_put) = NULL;
    struct robust_list_head *head;
    size_t len = sizeof(*head);

    if (tid == 0) {
        task = current;
        task_get(task);
    } else
        task = pid_get_task(tid, PIDTYPE_TID);

    if (!task)
        return -ESRCH;

    head = READ_ONCE(task->robust);
    if (copy_to_user(ulen, &len, sizeof(len)))
        return -EFAULT;

    /* copy robust_list_head pointer addr not robust_list_head struct */
    if (copy_to_user(uhead, &head, sizeof(head)))
        return -EFAULT;

    return 0;
}

long sys_set_robust_list(struct trap_frame *frame)
{
    struct robust_list_head *uhead =
        (struct robust_list_head *)syscallarg(frame, 0);
    size_t len = syscallarg(frame, 1);

    if (unlikely(len != sizeof(*uhead)))
        return -EINVAL;

    current->robust = uhead;

    return 0;
}

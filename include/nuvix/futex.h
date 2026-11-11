#ifndef _NUVIX_FUTEX_H
#define _NUVIX_FUTEX_H

#include <nuvix/ctypes.h>
#include <nuvix/list.h>
#include <nuvix/spinlock.h>

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3

#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

#define FUTEX_WAIT_PRIVATE    (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE    (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE (FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG)

#define FUTEX_WAITERS         0x80000000U
#define FUTEX_OWNER_DIED      0x40000000U
#define FUTEX_TID_MASK        0x3fffffffU

#define ROBUST_LIST_LIMIT     2048

struct robust_list {
    struct robust_list *next;
};

struct robust_list_head {
    struct robust_list list;
    long offset;
    struct robust_list *pending;
};

struct futex_key {
    struct mm_struct *mm;
    uint32_t *addr;
};

#define FUTEX_KEY_INIT  (struct futex_key){ .mm = NULL }

struct futex_waiter {
    struct futex_key key;
    struct list_head node;
    struct task *task;
    bool woken;
};

int __futex_cmpxchg_u32(uint32_t *obs, uint32_t *addr, uint32_t val, uint32_t desired);

extern char __futex_cmpxchg_load32[], __futex_cmpxchg_store32[];
extern char __futex_cmpxchg_fixup[];

void futex_init(void);

void futex_release(void);

#endif /* ifndef _NUVIX_FUTEX_H */

#include <nuvix/futex.h>
#include <nuvix/errno.h>
#include <nuvix/list.h>
#include <nuvix/mm.h>
#include <nuvix/mutex.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/wait.h>
#include <nuvix/processor.h>

#define FUTEX_BUCKETS		32
#define ROBUST_LIST_LIMIT	2048

struct futex_key {
	struct mm_struct *mm;
	uintptr_t uaddr;
};

struct futex_waiter {
	struct futex_key key;
	struct list_head node;
	struct task_struct *task;
	uint64_t generation;
	uint32_t bitset;
	bool woken;
};

struct futex_bucket {
	spinlock_t lock;
	struct list_head waiters;
};

static struct futex_bucket futex_buckets[FUTEX_BUCKETS];

static void futex_waiter_detach(struct futex_bucket *bucket,
				struct futex_waiter *waiter)
{
	irq_flags_t flags;

	if (!bucket || !waiter)
		return;
	spin_lock_irqsave(&bucket->lock, &flags);
	if (!list_empty(&waiter->node))
		list_del_init(&waiter->node);
	spin_unlock_irqrestore(&bucket->lock, flags);
}

void futex_init(void)
{
	for (int i = 0; i < FUTEX_BUCKETS; i++) {
		/* Bucket lock ranks with wait channels: wake paths hand a
		 * target off to wait_wake_event() outside the bucket lock. */
		spin_lock_init(&futex_buckets[i].lock, LOCK_RANK_WAIT_CHANNEL);
		INIT_LIST_HEAD(&futex_buckets[i].waiters);
	}
}

static bool futex_key_equal(const struct futex_key *a,
			    const struct futex_key *b)
{
	return a->mm == b->mm && a->uaddr == b->uaddr;
}

static struct futex_bucket *futex_bucket_for(const struct futex_key *key)
{
	uintptr_t hash = ((uintptr_t)key->mm >> 3) ^ (key->uaddr >> 2);

	return &futex_buckets[hash & (FUTEX_BUCKETS - 1)];
}

static int futex_make_key(struct mm_struct *mm, int *uaddr,
			  struct futex_key *key)
{
	if (!mm || !uaddr)
		return -EFAULT;
	if ((uintptr_t)uaddr & (sizeof(int) - 1))
		return -EINVAL;

	key->mm = mm;
	key->uaddr = (uintptr_t)uaddr;
	return 0;
}

static int futex_read_user_value_checked(int *uaddr, int *value)
{
	if (!uaddr || !value || !access_ok(uaddr, sizeof(*uaddr)))
		return -EFAULT;
	if (copy_from_user(value, uaddr, sizeof(*value)) != 0)
		return -EFAULT;
	return 0;
}

static int futex_wait(int *uaddr, int expected, uint32_t bitset,
			      const struct wait_deadline *deadline)
{
	struct futex_key key;
	struct futex_bucket *bucket;
	struct futex_waiter waiter;
	struct task_wait *wait = &current_task()->wait;
	wait_outcome_t outcome;
	irq_flags_t flags;
	int value;
	int ret;

	if (bitset == 0)
		return -EINVAL;

	ret = futex_make_key(current_task()->proc ? current_task()->proc->mm : NULL,
			    uaddr, &key);
	if (ret < 0)
		return ret;
	if (user_range_probe(uaddr, sizeof(*uaddr), false) < 0)
		return -EFAULT;

	bucket = futex_bucket_for(&key);
	ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, deadline);
	if (ret < 0)
		return ret;
	memset(&waiter, 0, sizeof(waiter));
	waiter.key = key;
	waiter.bitset = bitset;
	waiter.task = current_task();
	waiter.generation = wait->generation;
	INIT_LIST_HEAD(&waiter.node);
	ret = futex_read_user_value_checked(uaddr, &value);
	if (ret < 0)
		goto finish_wait;
	spin_lock_irqsave(&bucket->lock, &flags);
	if (value != expected) {
		spin_unlock_irqrestore(&bucket->lock, flags);
		ret = -EAGAIN;
		goto finish_wait;
	}
	list_add_tail(&waiter.node, &bucket->waiters);
	spin_unlock_irqrestore(&bucket->lock, flags);
	ret = futex_read_user_value_checked(uaddr, &value);
	if (ret < 0)
		goto detach_waiter;
	if (value != expected) {
		ret = -EAGAIN;
		goto detach_waiter;
	}
	ret = wait_block(wait, &outcome);

detach_waiter:
	futex_waiter_detach(bucket, &waiter);

finish_wait:
	wait_finish(wait);

	if (ret < 0)
		return ret;
	if (outcome == WAIT_OUTCOME_EVENT)
		return 0;
	if (outcome == WAIT_OUTCOME_SIGNAL)
		return -EINTR;
	if (outcome == WAIT_OUTCOME_TIMEOUT)
		return -ETIMEDOUT;
	return -EINVAL;
}

static int futex_wake_mm_bitset(struct mm_struct *mm, int *uaddr, int nr,
				uint32_t bitset)
{
	struct futex_key key;
	struct futex_bucket *bucket;
	irq_flags_t flags;
	int ret;
	int woken = 0;

	if (bitset == 0)
		return -EINVAL;
	if (nr <= 0)
		return 0;

	ret = futex_make_key(mm, uaddr, &key);
	if (ret < 0)
		return ret;

	bucket = futex_bucket_for(&key);

	while (woken < nr) {
		struct task_struct *target = NULL;
		struct list_head *pos;
		uint64_t generation = 0;

		spin_lock_irqsave(&bucket->lock, &flags);
		list_for_each (pos, &bucket->waiters) {
			struct futex_waiter *waiter =
				list_entry(pos, struct futex_waiter, node);

			if (!futex_key_equal(&waiter->key, &key))
				continue;
			if ((waiter->bitset & bitset) == 0)
				continue;
			if (waiter->woken)
				continue;
			if (!task_try_get(waiter->task))
				continue;
			waiter->woken = true;
			list_del_init(&waiter->node);
			target = waiter->task;
			generation = waiter->generation;
			break;
		}
		spin_unlock_irqrestore(&bucket->lock, flags);

		if (!target)
			break;
		if (wait_wake_event(target, generation))
			woken++;
		task_put(target);
	}

	return woken;
}

int futex_wake_mm(struct mm_struct *mm, int *uaddr, int nr)
{
	return futex_wake_mm_bitset(mm, uaddr, nr, FUTEX_BITSET_MATCH_ANY);
}

static int futex_wake(int *uaddr, int nr, uint32_t bitset)
{
	struct futex_key key;
	int ret;

	if (bitset == 0)
		return -EINVAL;
	ret = futex_make_key(current_task()->proc ? current_task()->proc->mm : NULL,
			    uaddr, &key);
	if (ret < 0)
		return ret;
	if (!access_ok(uaddr, sizeof(*uaddr)))
		return -EFAULT;

	return futex_wake_mm_bitset(key.mm, uaddr, nr, bitset);
}

static void robust_wake_owner(struct task_struct *task,
			      struct robust_list *node, long futex_offset)
{
	uintptr_t addr;
	int old_value;
	int new_value;
	int *uaddr;

	if (!task || !task->proc || !task->proc->mm || !node)
		return;

	addr = (uintptr_t)node + (uintptr_t)futex_offset;
	if (addr & (sizeof(int) - 1))
		return;
	uaddr = (int *)addr;
	if (copy_from_user(&old_value, uaddr, sizeof(old_value)) != 0)
		return;
	if (((uint32_t)old_value & FUTEX_TID_MASK) !=
	    (uint32_t)(task->tid ? task->tid->nr : 0))
		return;

	new_value = (old_value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
	if (copy_to_user(uaddr, &new_value, sizeof(new_value)) != 0)
		return;

	if (futex_wake_mm(task->proc->mm, uaddr, 1) < 0)
		return;
}

void futex_exit_robust_list(struct task_struct *task)
{
	struct robust_list_head head;
	struct robust_list_head *head_ptr;
	struct robust_list *entry;
	struct robust_list *pending;

	head_ptr = task_robust_list(task);
	if (!task || !head_ptr)
		return;
	if (task_robust_list_len(task) != sizeof(struct robust_list_head))
		return;
	if (copy_from_user(&head, head_ptr, sizeof(head)) != 0)
		return;

	entry = head.list.next;
	for (int i = 0;
	     entry && entry != &head_ptr->list && i < ROBUST_LIST_LIMIT; i++) {
		struct robust_list current_rb;

		robust_wake_owner(task, entry, head.futex_offset);
		if (copy_from_user(&current_rb, entry, sizeof(current_rb)) != 0)
			break;
		entry = current_rb.next;
	}

	pending = head.list_op_pending;
	if (pending && pending != &head_ptr->list)
		robust_wake_owner(task, pending, head.futex_offset);
}

int futex_set_robust_list(struct task_struct *task,
			  struct robust_list_head *head, size_t len)
{
	irq_flags_t flags;

	if (len != sizeof(struct robust_list_head))
		return -EINVAL;
	if (!task)
		return -EFAULT;

	spin_lock_irqsave(&task->wait.lock, &flags);
	task_set_robust_list(task, head, len);
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return 0;
}

int futex_get_robust_list(struct task_struct *task,
			  struct robust_list_head **head, size_t *len)
{
	irq_flags_t flags;

	spin_lock_irqsave(&task->wait.lock, &flags);
	*head = task_robust_list(task);
	*len = task_robust_list_len(task);
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return 0;
}

int kernel_futex(const struct kernel_futex_args *args)
{
	int cmd;

	cmd = args->op & FUTEX_CMD_MASK;
	if (args->op & FUTEX_CLOCK_REALTIME)
		return -ENOSYS;

	switch (cmd) {
	case FUTEX_WAIT:
		return futex_wait(args->uaddr, args->val,
				  FUTEX_BITSET_MATCH_ANY, args->deadline);
	case FUTEX_WAKE:
		return futex_wake(args->uaddr, args->val,
				  FUTEX_BITSET_MATCH_ANY);
	case FUTEX_WAIT_BITSET:
		return futex_wait(args->uaddr, args->val, (uint32_t)args->val3,
				  args->deadline);
	case FUTEX_WAKE_BITSET:
		return futex_wake(args->uaddr, args->val, (uint32_t)args->val3);
	default:
		return -ENOSYS;
	}
}

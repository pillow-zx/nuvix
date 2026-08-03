#include <kernel/futex.h>
#include <kernel/errno.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <kernel/slab.h>
#include <kernel/task.h>
#include <kernel/wait.h>
#include <kernel/processor.h>
#include <kernel/uaccess_arch.h>

constexpr int32_t FUTEX_BUCKETS = 32;
constexpr int32_t ROBUST_LIST_LIMIT = 2048;

struct futex_key {
	struct mm_struct *mm;
	uintptr_t uaddr;
};

struct futex_waiter {
	struct futex_key key;
	struct wait_channel wait;
	struct list_head node;
	uint32_t bitset;
	bool woken;
	bool owner_ref;
	refcount_t refs;
};

struct futex_bucket {
	spinlock_t lock;
	struct list_head waiters;
};

struct futex_wait_ctx {
	struct futex_bucket *bucket;
	struct futex_waiter *waiter;
	int *uaddr;
	int expected;
};

static struct futex_bucket futex_buckets[FUTEX_BUCKETS];

static bool futex_waiter_get(struct futex_waiter *waiter)
{
	return waiter && refcount_inc_not_zero(&waiter->refs);
}

static void futex_waiter_put(struct futex_waiter *waiter)
{
	if (waiter && refcount_dec_and_test(&waiter->refs))
		kfree(waiter);
}

static void futex_waiter_detach(struct futex_wait_ctx *ctx)
{
	struct futex_waiter *waiter;
	struct futex_bucket *bucket;
	irq_flags_t flags;
	bool release_owner = false;

	if (!ctx || !ctx->waiter || !ctx->bucket)
		return;

	waiter = ctx->waiter;
	bucket = ctx->bucket;
	spin_lock_irqsave(&bucket->lock, &flags);
	if (!list_empty(&waiter->node))
		list_del_init(&waiter->node);
	if (waiter->owner_ref) {
		waiter->owner_ref = false;
		release_owner = true;
	}
	spin_unlock_irqrestore(&bucket->lock, flags);

	if (release_owner)
		futex_waiter_put(waiter);
}

static void futex_wait_cancel(void *arg)
{
	futex_waiter_detach(arg);
}

void futex_init(void)
{
	for (int i = 0; i < FUTEX_BUCKETS; i++) {
		spin_lock_init(&futex_buckets[i].lock);
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
	if (!access_ok(uaddr, sizeof(*uaddr)))
		return -EFAULT;

	bool had_sum = user_access_begin();

	*value = *(volatile int *)uaddr;
	user_access_end(had_sum);
	return 0;
}

static int futex_wait_check(struct wait_session *session, void *arg)
{
	struct futex_wait_ctx *ctx = arg;
	struct futex_waiter *waiter = ctx->waiter;
	irq_flags_t flags;
	int ret;
	int value;
	bool newly_queued = false;

	spin_lock_irqsave(&ctx->bucket->lock, &flags);
	if (waiter->woken) {
		ret = 1;
		goto out;
	}

	if (list_empty(&waiter->node)) {
		ret = futex_read_user_value_checked(ctx->uaddr, &value);
		if (ret < 0)
			goto out;
		if (value != ctx->expected) {
			ret = -EAGAIN;
			goto out;
		}
		list_add_tail(&waiter->node, &ctx->bucket->waiters);
		newly_queued = true;
	}

	ret = wait_session_watch(session, &waiter->wait);
	if (ret < 0 && newly_queued)
		list_del_init(&waiter->node);
out:
	spin_unlock_irqrestore(&ctx->bucket->lock, flags);
	return ret;
}

static int futex_wait(int *uaddr, int expected, uint32_t bitset,
		      const struct wait_deadline *deadline)
{
	struct futex_key key;
	struct futex_bucket *bucket;
	struct futex_waiter *waiter;
	struct futex_wait_ctx wait_ctx;
	struct wait_request source = {.kind = WAIT_KIND_FUTEX};
	wait_outcome_t outcome;
	int ret;

	if (bitset == 0)
		return -EINVAL;

	ret = futex_make_key(task_mm(current_task()), uaddr, &key);
	if (ret < 0)
		return ret;
	if (user_range_probe(uaddr, sizeof(*uaddr), false) < 0)
		return -EFAULT;

	bucket = futex_bucket_for(&key);
	waiter = kmalloc(sizeof(*waiter));
	if (!waiter)
		return -ENOMEM;
	memset(waiter, 0, sizeof(*waiter));
	waiter->key = key;
	waiter->bitset = bitset;
	waiter->owner_ref = true;
	refcount_set(&waiter->refs, 1);
	wait_channel_init(&waiter->wait);
	INIT_LIST_HEAD(&waiter->node);
	wait_ctx.bucket = bucket;
	wait_ctx.waiter = waiter;
	wait_ctx.uaddr = uaddr;
	wait_ctx.expected = expected;
	source.kind = WAIT_KIND_FUTEX;
	source.check = futex_wait_check;
	source.cancel = futex_wait_cancel;
	source.arg = &wait_ctx;
	source.channel_limit = 1;

	ret = wait_for(&source, WAIT_FLAG_INTERRUPTIBLE, deadline, &outcome);

	futex_waiter_detach(&wait_ctx);

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
		struct futex_waiter *selected = NULL;
		struct list_head *pos;

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
			BUG_ON(!futex_waiter_get(waiter));
			waiter->woken = true;
			selected = waiter;
			break;
		}
		spin_unlock_irqrestore(&bucket->lock, flags);

		if (!selected)
			break;
		if (wait_channel_wake_one(&selected->wait))
			woken++;
		futex_waiter_put(selected);
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
	ret = futex_make_key(task_mm(current_task()), uaddr, &key);
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

	if (!task || !task_mm(task) || !node)
		return;

	addr = (uintptr_t)node + (uintptr_t)futex_offset;
	if (addr & (sizeof(int) - 1))
		return;
	uaddr = (int *)addr;
	if (copy_from_user(&old_value, uaddr, sizeof(old_value)) != 0)
		return;
	if (((uint32_t)old_value & FUTEX_TID_MASK) != (uint32_t)task_pid(task))
		return;

	new_value = (old_value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
	if (copy_to_user(uaddr, &new_value, sizeof(new_value)) != 0)
		return;

	if (futex_wake_mm(task_mm(task), uaddr, 1) < 0)
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
	if (len != sizeof(struct robust_list_head))
		return -EINVAL;
	if (!task)
		return -EFAULT;

	task_set_robust_list(task, head, len);
	return 0;
}

int futex_get_robust_list(struct task_struct *task,
			  struct robust_list_head **head, size_t *len)
{
	*head = task_robust_list(task);
	*len = task_robust_list_len(task);
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

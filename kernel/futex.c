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

static void futex_waiter_detach(struct futex_bucket *bucket, struct futex_waiter *waiter)
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
		spin_lock_init(&futex_buckets[i].lock, LOCK_RANK_FUTEX_BUCKET,
				LOCK_IRQ_TASK_ONLY);
		INIT_LIST_HEAD(&futex_buckets[i].waiters);
	}
}

static bool futex_key_equal(const struct futex_key *a,
			    const struct futex_key *b)
{
	if (a->shared != b->shared)
		return false;
	if (a->shared)
		return a->shared_file.mapping == b->shared_file.mapping &&
		       a->shared_file.pgoff == b->shared_file.pgoff;
	return a->priv.mm == b->priv.mm &&
	       a->priv.uaddr == b->priv.uaddr;
}

static struct futex_bucket *futex_bucket_for(const struct futex_key *key)
{
	uintptr_t hash;

	if (key->shared)
		hash = ((uintptr_t)key->shared_file.mapping >> 3) ^
		       key->shared_file.pgoff;
	else
		hash = ((uintptr_t)key->priv.mm >> 3) ^
		       (key->priv.uaddr >> 2);

	return &futex_buckets[hash & (FUTEX_BUCKETS - 1)];
}

void futex_key_put(struct futex_key *key)
{
	if (!key)
		return;
	if (key->shared)
		mm_map_id_put(&key->shared_file);
	else
		mm_put(key->priv.mm);
	memset(key, 0, sizeof(*key));
}

static int futex_key_fill(struct futex_key *key, struct mm_struct *mm,
			  uintptr_t uaddr, const struct mm_map_id *id,
			  bool mm_owned)
{
	memset(key, 0, sizeof(*key));
	switch (id->kind) {
	case MM_MAPPING_PRIVATE:
		if (!mm_owned)
			mm_get(mm);
		key->priv.mm = mm;
		key->priv.uaddr = uaddr;
		return 0;
	case MM_MAPPING_SHARED_FILE:
		key->shared = true;
		/* The key takes ownership of the held file reference. */
		key->shared_file = *id;
		return 0;
	case MM_MAPPING_SHARED_ANON:
		return -ENOSYS;
	default:
		return -EFAULT;
	}
}

int futex_key_init_locked(struct futex_key *key, struct mm_struct *mm, uintptr_t uaddr)
{
	struct mm_map_id id;
	int ret;

	if (!mm || !uaddr)
		return -EFAULT;
	if (uaddr & (sizeof(int) - 1))
		return -EINVAL;

	memset(key, 0, sizeof(*key));
	ret = mm_map_id_get_locked(mm, uaddr, &id);
	if (ret < 0)
		return ret;
	return futex_key_fill(key, mm, uaddr, &id, false);
}

int futex_key_init(struct futex_key *key, struct mm_struct *mm,
			   uintptr_t uaddr)
{
	struct mm_map_id id;
	int ret;

	if (!mm)
		return -EFAULT;
	mm_get(mm);
	ret = mm_map_id_get(mm, uaddr, &id);
	if (ret < 0) {
		mm_put(mm);
		return ret;
	}
	ret = futex_key_fill(key, mm, uaddr, &id, true);
	if (ret < 0 || key->shared)
		mm_put(mm);
	return ret;
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
	struct task_struct *task = current_task();
	struct proc_struct *proc = task->proc;
	struct task_wait *wait = &task->wait;
	wait_outcome_t outcome;
	irq_flags_t flags;
	int value;
	int ret;

	if (bitset == 0)
		return -EINVAL;

	ret = futex_key_init(&key, proc ? proc->mm : NULL,
			    (uintptr_t)uaddr);
	if (ret < 0)
		return ret;
	if (user_range_probe(uaddr, sizeof(*uaddr), false) < 0) {
		futex_key_put(&key);
		return -EFAULT;
	}

	bucket = futex_bucket_for(&key);
	ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, deadline);
	if (ret < 0) {
		futex_key_put(&key);
		return ret;
	}
	memset(&waiter, 0, sizeof(waiter));
	waiter.key = key;
	memset(&key, 0, sizeof(key));
	waiter.bitset = bitset;
	waiter.task = task;
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
	futex_key_put(&waiter.key);

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

static int futex_wake_key_bitset(const struct futex_key *key, int nr,
				 uint32_t bitset)
{
	struct futex_bucket *bucket;
	irq_flags_t flags;
	int woken = 0;

	if (bitset == 0)
		return -EINVAL;
	if (nr <= 0)
		return 0;

	bucket = futex_bucket_for(key);

	while (woken < nr) {
		struct task_struct *target = NULL;
		struct list_head *pos;
		uint64_t generation = 0;

		spin_lock_irqsave(&bucket->lock, &flags);
		list_for_each (pos, &bucket->waiters) {
			struct futex_waiter *waiter =
				list_entry(pos, struct futex_waiter, node);

			if (!futex_key_equal(&waiter->key, key))
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

static int futex_wake_mm_bitset(struct mm_struct *mm, int *uaddr, int nr,
				uint32_t bitset)
{
	struct futex_key key;
	int ret;

	if (bitset == 0)
		return -EINVAL;

	ret = futex_key_init(&key, mm, (uintptr_t)uaddr);
	if (ret < 0)
		return ret;
	if (nr <= 0) {
		futex_key_put(&key);
		return 0;
	}
	ret = futex_wake_key_bitset(&key, nr, bitset);
	futex_key_put(&key);
	return ret;
}

int futex_wake_key(const struct futex_key *key, int nr)
{
	return futex_wake_key_bitset(key, nr, FUTEX_BITSET_MATCH_ANY);
}

int futex_wake_mm(struct mm_struct *mm, int *uaddr, int nr)
{
	return futex_wake_mm_bitset(mm, uaddr, nr, FUTEX_BITSET_MATCH_ANY);
}

static int futex_wake(int *uaddr, int nr, uint32_t bitset)
{
	if (bitset == 0)
		return -EINVAL;
	if (!access_ok(uaddr, sizeof(*uaddr)))
		return -EFAULT;

	return futex_wake_mm_bitset(
		current_task()->proc ? current_task()->proc->mm : NULL, uaddr,
		nr, bitset);
}

static int robust_futex_address(const struct robust_list *node,
				long futex_offset, uintptr_t *address)
{
	uintptr_t node_addr;
	uintptr_t offset;

	if (!node || !address)
		return -EFAULT;
	node_addr = (uintptr_t)node;
	if (node_addr & (sizeof(uintptr_t) - 1))
		return -EINVAL;
	if (futex_offset >= 0) {
		offset = (uintptr_t)futex_offset;
		if (check_add_overflow(node_addr, offset, address))
			return -EINVAL;
	} else {
		offset = (uintptr_t)(-(futex_offset + 1)) + 1;
		if (check_sub_overflow(node_addr, offset, address))
			return -EINVAL;
	}
	if (*address & (sizeof(uint32_t) - 1) ||
	    !access_ok((const void *)*address, sizeof(uint32_t)))
		return -EINVAL;
	return 0;
}

static int robust_commit_owner_dead(struct mm_struct *mm, uint32_t dead_tid,
					uintptr_t address)
{
	struct uaccess_txn txn;
	struct futex_key key __cleanup_with(futex_key_ref) = {};
	uint32_t old_value;
	uint32_t observed;
	uint32_t owner = dead_tid & FUTEX_TID_MASK;
	uint32_t desired;
	int ret;

	ret = uaccess_begin_mm(&txn, mm);
	if (ret < 0)
		return ret;
	ret = futex_key_init_locked(&key, mm, address);
	if (ret < 0) {
		uaccess_end(&txn);
		return ret;
	}
	ret = uaccess_load_u32(&txn, (const volatile uint32_t *)address,
					 &old_value);
	if (ret == 0 && (old_value & FUTEX_TID_MASK) != owner)
		ret = -EAGAIN;
	if (ret == 0) {
		desired = (old_value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
		ret = uaccess_cmpxchg_u32(&txn, (volatile uint32_t *)address,
					     old_value, desired, &observed);
	}
	uaccess_end(&txn);
	if (ret == 0) {
		int wake_ret = futex_wake_key(&key, 1);

		(void)wake_ret;
	}
	return ret;
}

static int robust_owner_dead(struct mm_struct *mm, uint32_t dead_tid,
				     const struct robust_list *node,
				     long futex_offset)
{
	uintptr_t address;
	int ret;

	ret = robust_futex_address(node, futex_offset, &address);
	if (ret < 0)
		return ret;
	return robust_commit_owner_dead(mm, dead_tid, address);
}

void futex_exit_robust_list(struct task_struct *task)
{
	struct robust_list_head head;
	struct robust_list_head *head_ptr;
	struct robust_list *entry;
	struct robust_list *pending;
	struct mm_struct *mm __cleanup_with(mm_ref) = NULL;
	uintptr_t *visited __cleanup_with(kfree) = NULL;
	uint32_t dead_tid;
	size_t visited_count = 0;
	irq_flags_t flags;

	if (!task || task_is_idle(task) || !task->proc)
		return;
	spin_lock_irqsave(&task->wait.lock, &flags);
	if (task->signal.robust_cleanup_done) {
		spin_unlock_irqrestore(&task->wait.lock, flags);
		return;
	}
	task->signal.robust_cleanup_done = true;
	head_ptr = task->signal.robust_list;
	if (task->signal.robust_list_len != sizeof(struct robust_list_head)) {
		spin_unlock_irqrestore(&task->wait.lock, flags);
		return;
	}
	spin_unlock_irqrestore(&task->wait.lock, flags);
	if (!head_ptr || ((uintptr_t)head_ptr & (sizeof(uintptr_t) - 1)) ||
	    !access_ok(head_ptr, sizeof(head)))
		return;
	mm = proc_mm_get(task->proc);
	if (!mm)
		return;
	if (uaccess_copy_from_mm(mm, &head, head_ptr, sizeof(head)) < 0)
		return;
	visited = kmalloc_array(ROBUST_LIST_LIMIT, sizeof(*visited),
				ALLOC_NOWAIT);
	dead_tid = task->tid ? (uint32_t)task->tid->nr : 0;

	entry = head.list.next;
	for (int i = 0;
	     visited && entry && entry != &head_ptr->list &&
	     i < ROBUST_LIST_LIMIT; i++) {
		uintptr_t address = (uintptr_t)entry;
		struct robust_list *next;
		bool duplicate = false;

		if ((address & (sizeof(uintptr_t) - 1)) ||
		    !access_ok(entry, sizeof(entry->next)))
			break;
		for (size_t j = 0; j < visited_count; j++)
			if (visited[j] == address) {
				duplicate = true;
				break;
			}
		if (duplicate)
			break;
		visited[visited_count++] = address;
		(void)robust_owner_dead(mm, dead_tid, entry, head.futex_offset);
		if (uaccess_copy_from_mm(mm, &next, &entry->next,
					 sizeof(next)) < 0)
			break;
		entry = next;
	}

	pending = head.list_op_pending;
	if (pending && pending != &head_ptr->list)
		(void)robust_owner_dead(mm, dead_tid, pending, head.futex_offset);
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
	if (task->lifecycle >= TASK_EXITING) {
		spin_unlock_irqrestore(&task->wait.lock, flags);
		return -ESRCH;
	}
	task_set_robust_list(task, head, len);
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return 0;
}

int futex(const struct futex_args *args)
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

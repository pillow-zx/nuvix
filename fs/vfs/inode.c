#include <nuvix/vfs.h>

#include <nuvix/errno.h>
#include <nuvix/string.h>

static void inode_init(struct inode *inode, struct super_block *sb,
                       uint64_t ino)
{
    memset(inode, 0, sizeof(*inode));
    refcount_set(&inode->i_count, 1);
    inode->i_sb = sb;
    inode->i_ino = ino;
    INIT_HLIST_NODE(&inode->i_hash);
    inode->i_state = INODE_LOADING;
    mutex_init(&inode->i_wlock);
}

static struct inode *icache_find(struct icache *cache, uint64_t ino)
{
    struct inode *inode;

    assert_spin_locked(&cache->wait.lock);
    hash_for_each_possible (cache->entries, inode, i_hash, ino) {
        if (inode->i_ino == ino)
            return inode;
    }
    return NULL;
}

/* The caller's reference keeps a loading/failed object alive while waiting. */
static struct inode *inode_wait_ready(struct inode *ref)
{
    struct inode *inode __free(iput) = NULL;
    struct wait_queue *queue = &ref->i_sb->s_icache.wait;
    struct wait_entry wait;
    unsigned long flags;
    int error;

    inode = ref;
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&queue->lock, flags);
        finish_wait_locked(queue, &wait);
        if (inode->i_state != INODE_LOADING) {
            Assert(inode->i_state == INODE_READY ||
                       inode->i_state == INODE_FAILED,
                   "invalid referenced inode state\n");
            error = inode->i_load_error;
            spin_unlock_irqrestore(&queue->lock, flags);
            preempt_enable();
            break;
        }
        prepare_to_wait_locked(queue, &wait);
        spin_unlock_irqrestore(&queue->lock, flags);
        sched_park();
    }
    if (error)
        return ERR_PTR(error);
    return move(inode);
}

struct inode *iget(struct super_block *sb, uint64_t ino)
{
    struct icache *cache = &sb->s_icache;
    struct inode *candidate = NULL;
    struct inode *inode __free(iput) = NULL;
    struct inode *found = NULL;
    struct wait_entry wait;
    unsigned long flags;
    int error;

    might_sleep();
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&cache->wait.lock, flags);
        finish_wait_locked(&cache->wait, &wait);
        found = icache_find(cache, ino);
        if (found && found->i_state == INODE_FREEING) {
            /* No reference is available: recheck the key after waking. */
            prepare_to_wait_locked(&cache->wait, &wait);
            spin_unlock_irqrestore(&cache->wait.lock, flags);
            sched_park();
            continue;
        }
        if (found) {
            refcount_inc(&found->i_count);
            inode = found;
            spin_unlock_irqrestore(&cache->wait.lock, flags);
            preempt_enable();
            if (candidate)
                sb->s_ops->destroy_inode(candidate);
            return inode_wait_ready(move(inode));
        }
        if (candidate) {
            inode = move(candidate);
            hash_add(cache->entries, &inode->i_hash, ino);
            spin_unlock_irqrestore(&cache->wait.lock, flags);
            preempt_enable();
            break;
        }
        spin_unlock_irqrestore(&cache->wait.lock, flags);
        preempt_enable();

        candidate = sb->s_ops->alloc_inode(sb);
        if (!candidate)
            return ERR_PTR(-ENOMEM);
        inode_init(candidate, sb, ino);
    }

    error = sb->s_ops->read_inode(inode);
    spin_lock_irqsave(&cache->wait.lock, flags);
    inode->i_load_error = error;
    inode->i_state = error ? INODE_FAILED : INODE_READY;
    wake_up_all_locked(&cache->wait);
    spin_unlock_irqrestore(&cache->wait.lock, flags);
    if (error)
        return ERR_PTR(error);
    return move(inode);
}

void ihold(struct inode *inode)
{
    Assert(inode->i_state == INODE_READY, "ihold requires a ready inode\n");
    refcount_inc(&inode->i_count);
}

void iput(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct wait_queue *queue = &sb->s_icache.wait;
    unsigned long flags;

    might_sleep();
    spin_lock_irqsave(&queue->lock, flags);
    if (!refcount_dec_and_test(&inode->i_count)) {
        spin_unlock_irqrestore(&queue->lock, flags);
        return;
    }
    Assert(inode->i_state == INODE_READY || inode->i_state == INODE_FAILED,
           "invalid final inode state\n");
    inode->i_state = INODE_FREEING;
    spin_unlock_irqrestore(&queue->lock, flags);

    sb->s_ops->evict_inode(inode);

    spin_lock_irqsave(&queue->lock, flags);
    hash_del(&inode->i_hash);
    wake_up_all_locked(queue);
    spin_unlock_irqrestore(&queue->lock, flags);
    sb->s_ops->destroy_inode(inode);
}

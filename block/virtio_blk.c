/*
 * block/virtio_blk.c - virtio-blk MMIO 驱动（modern 传输层，中断完成）
 */

#include <drivers/virtio_blk.h>
#include <drivers/virtio.h>
#include <nuvix/blkdev.h>
#include <nuvix/string.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/spinlock.h>
#include <nuvix/mutex.h>
#include <nuvix/time.h>
#include <nuvix/irq.h>
#include <nuvix/tools.h>
#include <nuvix/page.h>
#include <nuvix/atomic.h>
#include <nuvix/worker.h>
#include <nuvix/timer.h>
#include <nuvix/dt.h>
#include <nuvix/mmio.h>

#define VBLK_QSIZE	     8
#define VBLK_MAX_SECTORS     256u
#define VBLK_SLOTS          (VBLK_QSIZE / 3)
#define VBLK_TIMEOUT_MS     30000u

struct vblk_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VBLK_QSIZE];
	uint16_t used_event;
};

struct vblk_used {
	uint16_t flags;
	volatile uint16_t idx;
	struct vring_used_elem ring[VBLK_QSIZE];
	uint16_t avail_event;
};

struct virtio_blk_req {
	struct virtio_blk_outhdr hdr;
	volatile uint8_t status;
	struct blk_request *active;
	uint64_t deadline;
};

struct virtio_blk_dev {
	uintptr_t mmio_base;
	uint64_t capacity;
	struct vring_desc desc[VBLK_QSIZE] __aligned(VRING_DESC_ALIGN_SIZE);
	struct vblk_avail avail __aligned(VRING_AVAIL_ALIGN_SIZE);
	struct vblk_used used __aligned(VRING_USED_ALIGN_SIZE);
	struct virtio_blk_req req[VBLK_SLOTS];
	spinlock_t completion_lock;
	struct list_head pending;
	bool flush_supported;
	bool resetting;
	bool needs_reset;
	uint16_t last_used;
	struct blkdev bdev;
};

static void vblk_status_set(vaddr_t base, uint32_t bits)
{
	virtio_mmio_write(base, VIRTIO_MMIO_STATUS, bits);
}

static uint32_t vblk_status_get(vaddr_t base)
{
	return virtio_mmio_read(base, VIRTIO_MMIO_STATUS);
}

static int virtio_blk_read_sectors(struct blkdev *bdev, void *buf,
				   uint64_t sector, uint32_t nsec);
static int virtio_blk_write_sectors(struct blkdev *bdev, const void *buf,
				    uint64_t sector, uint32_t nsec);
static int virtio_blk_submit(struct blkdev *bdev, struct blk_request *request);

static const struct blkdev_ops vblk_ops = {
	.submit = virtio_blk_submit,
	.read_sectors = virtio_blk_read_sectors,
	.write_sectors = virtio_blk_write_sectors,
};

static struct virtio_blk_dev vblk = {
	.completion_lock = SPINLOCK_INIT,
	.bdev = {
		.bd_dev = MKDEV(VIRTIO_BLK_MAJOR, 0),
		.bd_ops = &vblk_ops,
		.bd_private = &vblk,
	},
};

static void vblk_setup_queue(struct virtio_blk_dev *dev)
{
	vaddr_t base = dev->mmio_base;
	uint32_t qnum_max;

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_SEL, 0);

	qnum_max = virtio_mmio_read(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qnum_max < VBLK_QSIZE)
		panic("virtio-blk: queue too small (max=%u, need=%u)\n",
		      qnum_max, VBLK_QSIZE);

	memset(&dev->desc, 0, sizeof(dev->desc));
	memset(&dev->avail, 0, sizeof(dev->avail));
	memset(&dev->used, 0, sizeof(dev->used));

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_NUM, VBLK_QSIZE);

	virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_DESC_LOW, __pa(dev->desc));
	virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW,
			    __pa(&dev->avail));
	virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_USED_LOW,
			    __pa(&dev->used));

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_READY, 1);
}

static void vblk_negotiate_features(struct virtio_blk_dev *dev)
{
	vaddr_t base = dev->mmio_base;
	uint32_t status;
	uint32_t offered;

	virtio_mmio_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
	offered = virtio_mmio_read(base, VIRTIO_MMIO_DEVICE_FEATURES);
	dev->flush_supported = !!(offered & (1u << VIRTIO_BLK_F_FLUSH));

	virtio_mmio_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
	if (!(virtio_mmio_read(base, VIRTIO_MMIO_DEVICE_FEATURES) &
	      (1u << (VIRTIO_F_VERSION_1 - 32))))
		panic("virtio-blk: VERSION_1 is not offered");
	virtio_mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
	virtio_mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES,
			  dev->flush_supported ? (1u << VIRTIO_BLK_F_FLUSH) : 0);

	virtio_mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
	virtio_mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES,
			  1u << (VIRTIO_F_VERSION_1 - 32));

	status = VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
		 VIRTIO_CONFIG_S_FEATURES_OK;
	vblk_status_set(base, status);

	if (!(vblk_status_get(base) & VIRTIO_CONFIG_S_FEATURES_OK))
		panic("virtio-blk: feature negotiation failed (VERSION_1 "
		      "rejected)\n");
}

static void vblk_build_req(struct virtio_blk_dev *dev, unsigned int slot,
			   struct blk_request *request)
{
	struct virtio_blk_req *owned = &dev->req[slot];
	unsigned int base = slot * 3;
	bool flush = request->op == BLK_REQUEST_FLUSH;
	owned->active = request;
	owned->hdr.type = flush ? VIRTIO_BLK_T_FLUSH :
		request->op == BLK_REQUEST_WRITE ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	owned->hdr.ioprio = 0;
	owned->hdr.sector = flush ? 0 : request->sector;
	owned->status = 0xff;
	dev->desc[base].addr = __pa(&owned->hdr);
	dev->desc[base].len = sizeof(owned->hdr);
	dev->desc[base].flags = VRING_DESC_F_NEXT;
	dev->desc[base].next = flush ? base + 2 : base + 1;
	dev->desc[base + 1].addr = flush ? 0 : __pa(request->buffer);
	dev->desc[base + 1].len = flush ? 0 : request->nsec * SECTOR_SIZE;
	dev->desc[base + 1].flags = VRING_DESC_F_NEXT |
		(request->op == BLK_REQUEST_READ ? VRING_DESC_F_WRITE : 0);
	dev->desc[base + 1].next = base + 2;
	dev->desc[base + 2].addr = __pa(&owned->status);
	dev->desc[base + 2].len = sizeof(owned->status);
	dev->desc[base + 2].flags = VRING_DESC_F_WRITE;
	dev->desc[base + 2].next = 0;
}

/* Called with completion_lock held. Device-visible descriptors belong to
 * distinct slots, so a queue-full request waits without sleeping here. */
static void vblk_dispatch_locked(struct virtio_blk_dev *dev)
{
	bool notify = false;
	if (dev->resetting)
		return;
	for (unsigned int i = 0; i < VBLK_SLOTS && !list_empty(&dev->pending); i++) {
		struct blk_request *request;
		struct wait_deadline deadline;
		int ret;
		if (dev->req[i].active)
			continue;
		request = list_first_entry(&dev->pending, struct blk_request, node);
		list_del_init(&request->node);
		vblk_build_req(dev, i, request);
		ret = mtime_deadline_from_ms(VBLK_TIMEOUT_MS, &deadline);
		BUG_ON(ret < 0);
		dev->req[i].deadline = deadline.expires;
		dev->avail.ring[dev->avail.idx % VBLK_QSIZE] = i * 3;
		virtio_wmb();
		dev->avail.idx++;
		notify = true;
	}
	if (notify)
		virtio_mmio_write(dev->mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

static uint64_t vblk_read_capacity(vaddr_t base)
{
	/* A coherent configuration snapshot, not a device-completion poll. */
	for (unsigned retry = 0; retry < 8; retry++) {
		uint32_t generation = virtio_mmio_read(base, VIRTIO_MMIO_CONFIG_GENERATION);
		uint32_t low = virtio_mmio_read(base, VIRTIO_MMIO_CONFIG);
		uint32_t high = virtio_mmio_read(base, VIRTIO_MMIO_CONFIG + 4);
		if (generation == virtio_mmio_read(base, VIRTIO_MMIO_CONFIG_GENERATION))
			return (uint64_t)low | ((uint64_t)high << 32);
	}
	panic("virtio-blk: unstable device configuration");
}

static void vblk_handle_irq(unsigned irq, void *data)
{
	struct virtio_blk_dev *vd = data;
	vaddr_t base = vd->mmio_base;
	struct blk_request *completed[VBLK_SLOTS];
	int results[VBLK_SLOTS];
	unsigned int count = 0;
	irq_flags_t flags;
	(void)irq;

	uint32_t status = virtio_mmio_read(base, VIRTIO_MMIO_INTERRUPT_STATUS);
	/* Acknowledge first: completions arriving during the drain can assert
	 * a fresh interrupt. Used indices, not interrupt counts, own requests. */
	virtio_mmio_write(base, VIRTIO_MMIO_INTERRUPT_ACK,
			  status & (VIRTIO_MMIO_INT_VRING | VIRTIO_MMIO_INT_CONFIG));
	spin_lock_irqsave(&vd->completion_lock, flags);
	if (vd->resetting) {
		spin_unlock_irqrestore(&vd->completion_lock, flags);
		return;
	}
	if ((status & VIRTIO_MMIO_INT_CONFIG) &&
	    vblk_read_capacity(base) != vd->capacity)
		panic("virtio-blk: runtime capacity changes are not supported");
	if (vblk_status_get(base) & VIRTIO_CONFIG_S_NEEDS_RESET) {
		vd->needs_reset = true;
		spin_unlock_irqrestore(&vd->completion_lock, flags);
		return;
	}
	uint16_t used = vd->used.idx;
	virtio_rmb();
	while (used != vd->last_used) {
		struct vring_used_elem *entry =
			&vd->used.ring[vd->last_used % VBLK_QSIZE];
		unsigned int slot = entry->id / 3;
		if (entry->id % 3 || slot >= VBLK_SLOTS ||
		    !vd->req[slot].active || count >= VBLK_SLOTS)
			panic("virtio-blk: invalid used ring completion");
		completed[count] = vd->req[slot].active;
		results[count++] = vd->req[slot].status == VIRTIO_BLK_S_OK ? 0 : -EIO;
		vd->req[slot].active = NULL;
		vd->last_used++;
	}
	vblk_dispatch_locked(vd);
	spin_unlock_irqrestore(&vd->completion_lock, flags);
	for (unsigned int i = 0; i < count; i++)
		completed[i]->complete(completed[i], results[i]);
}

static void vblk_watchdog_once(void *arg)
{
	struct virtio_blk_dev *dev = arg;
	struct blk_request *failed[VBLK_SLOTS];
	unsigned int count = 0;
	irq_flags_t flags;
	bool expired = false;
	uint64_t now = timer_now();
	spin_lock_irqsave(&dev->completion_lock, flags);
	if (!dev->resetting) {
		expired = dev->needs_reset;
		for (unsigned int i = 0; i < VBLK_SLOTS; i++)
			if (dev->req[i].active && now >= dev->req[i].deadline)
				expired = true;
	}
	if (!expired) {
		spin_unlock_irqrestore(&dev->completion_lock, flags);
		return;
	}
	dev->resetting = true;
	for (unsigned int i = 0; i < VBLK_SLOTS; i++) {
		if (dev->req[i].active)
			failed[count++] = dev->req[i].active;
		dev->req[i].active = NULL;
	}
	spin_unlock_irqrestore(&dev->completion_lock, flags);

	/* Reset quiesces DMA before a failed request's owner may free its data. */
	vblk_status_set(dev->mmio_base, 0);
	for (unsigned int i = 0; vblk_status_get(dev->mmio_base) != 0; i++)
		if (i == 1000000)
			panic("virtio-blk: reset did not quiesce DMA");
	vblk_status_set(dev->mmio_base, VIRTIO_CONFIG_S_ACKNOWLEDGE);
	vblk_status_set(dev->mmio_base,
			VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
	vblk_negotiate_features(dev);
	vblk_setup_queue(dev);
	virtio_mmio_write(dev->mmio_base, VIRTIO_MMIO_INTERRUPT_ACK,
			  virtio_mmio_read(dev->mmio_base,
					   VIRTIO_MMIO_INTERRUPT_STATUS));
	vblk_status_set(dev->mmio_base,
			VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
			VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
	spin_lock_irqsave(&dev->completion_lock, flags);
	dev->last_used = 0;
	dev->needs_reset = false;
	dev->resetting = false;
	vblk_dispatch_locked(dev);
	spin_unlock_irqrestore(&dev->completion_lock, flags);
	for (unsigned int i = 0; i < count; i++)
		failed[i]->complete(failed[i], -ETIMEDOUT);
}

void virtio_blk_watchdog_thread(void *arg)
{
	(void)arg;
	worker_run_periodic(1, vblk_watchdog_once, &vblk);
}

static int virtio_blk_submit(struct blkdev *bdev, struct blk_request *request)
{
	struct virtio_blk_dev *dev = bdev->bd_private;
	irq_flags_t flags;
	if (!request || !request->complete)
		return -EINVAL;
	if (request->op == BLK_REQUEST_FLUSH) {
		if (!dev->flush_supported)
			return -EOPNOTSUPP;
	} else if (!request->buffer || !request->nsec ||
		   request->nsec > VBLK_MAX_SECTORS ||
		   request->nsec > dev->capacity ||
		   request->sector > dev->capacity - request->nsec) {
		return -EINVAL;
	}
	INIT_LIST_HEAD(&request->node);
	spin_lock_irqsave(&dev->completion_lock, flags);
	list_add_tail(&request->node, &dev->pending);
	vblk_dispatch_locked(dev);
	spin_unlock_irqrestore(&dev->completion_lock, flags);
	return 0;
}

struct vblk_sync {
	struct blk_request request;
	struct wait_channel completion;
	spinlock_t completion_lock;
	bool done;
	int result;
};

static bool vblk_sync_done(struct vblk_sync *sync)
{
	irq_flags_t flags;
	bool done;
	spin_lock_irqsave(&sync->completion_lock, flags);
	done = sync->done;
	spin_unlock_irqrestore(&sync->completion_lock, flags);
	return done;
}

static void vblk_sync_complete(struct blk_request *request, int result)
{
	struct vblk_sync *sync = request->private_data;
	irq_flags_t flags;
	spin_lock_irqsave(&sync->completion_lock, flags);
	sync->result = result;
	sync->done = true;
	wait_channel_wake_all(&sync->completion);
	spin_unlock_irqrestore(&sync->completion_lock, flags);
}

static int virtio_blk_sync(struct blkdev *bdev, enum blk_request_op op,
			   void *buf, uint64_t sector, uint32_t nsec)
{
	struct vblk_sync sync = {
		.request = {.op = op, .buffer = buf, .sector = sector,
			    .nsec = nsec, .complete = vblk_sync_complete},
	};
	const struct wait_deadline deadline = wait_deadline_none();
	int ret;
	if (!wait_may_block())
		return -EINVAL;
	wait_channel_init(&sync.completion);
	spin_lock_init(&sync.completion_lock);
	sync.request.private_data = &sync;
	ret = virtio_blk_submit(bdev, &sync.request);
	if (ret < 0)
		return ret;
	while (!vblk_sync_done(&sync)) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		ret = wait_scope_begin(&scope, 0, &deadline);
		BUG_ON(ret < 0);
		ret = wait_scope_prepare(&scope, &sync.completion, false);
		BUG_ON(ret < 0);
		if (!vblk_sync_done(&sync)) {
			ret = wait_scope_block(&scope, &outcome);
			BUG_ON(ret < 0);
		}
		wait_scope_complete(&scope);
	}
	return sync.result;
}

static int virtio_blk_read_sectors(struct blkdev *bdev, void *buf,
				   uint64_t sector, uint32_t nsec)
{
	return virtio_blk_sync(bdev, BLK_REQUEST_READ, buf, sector, nsec);
}

static int virtio_blk_write_sectors(struct blkdev *bdev, const void *buf,
				    uint64_t sector, uint32_t nsec)
{
	return virtio_blk_sync(bdev, BLK_REQUEST_WRITE, (void *)buf, sector, nsec);
}

vaddr_t virtio_blk_probe(int node)
{
	struct dt_resource resource;
	if (dt_reg(node, 0, &resource) || resource.size < VIRTIO_MMIO_CONFIG + 8 ||
	    (resource.start & 3))
		panic("virtio: invalid MMIO resource");
	vaddr_t base = mmio_map(resource.start, resource.size);
	uint32_t magic = virtio_mmio_read(base, VIRTIO_MMIO_MAGIC_VALUE);
	if (magic != VIRTIO_MMIO_MAGIC)
		panic("virtio-blk: bad magic 0x%x at 0x%lx (expected 0x%x)\n",
		      magic, base, VIRTIO_MMIO_MAGIC);

	if (virtio_mmio_read(base, VIRTIO_MMIO_DEVICE_ID) != 2)
		return 0;
	dt_require_simple_device(node, true);
	uint32_t version = virtio_mmio_read(base, VIRTIO_MMIO_VERSION);
	if (version != 2)
		panic("virtio-blk: unsupported transport version %u (need "
		      "modern=2)\n",
		      version);
	return base;
}

dev_t virtio_blk_init(vaddr_t base, unsigned irq)
{
	struct virtio_blk_dev *dev = &vblk;
	vblk_status_set(base, 0);
	if (vblk_status_get(base) != 0)
		panic("virtio-blk: transport did not reset");
	dev->mmio_base = base;
	INIT_LIST_HEAD(&dev->pending);
	vblk_status_set(base, VIRTIO_CONFIG_S_ACKNOWLEDGE);
	vblk_status_set(base, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
	vblk_negotiate_features(dev);
	vblk_setup_queue(dev);
	dev->capacity = vblk_read_capacity(base);
	dev->bdev.bd_sectors = dev->capacity;

	int ret = irq_register(irq, 0, vblk_handle_irq, dev);
	if (ret)
		panic("virtio-blk: IRQ registration failed (%d)", ret);
	virtio_mmio_write(base, VIRTIO_MMIO_INTERRUPT_ACK,
			  virtio_mmio_read(base, VIRTIO_MMIO_INTERRUPT_STATUS));
	ret = irq_enable(irq);
	if (ret)
		panic("virtio-blk: IRQ enable failed (%d)", ret);
	vblk_status_set(base, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
			    VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
	ret = register_blkdev(&dev->bdev);
	if (ret)
		panic("virtio-blk: registration failed (%d)", ret);
	return dev->bdev.bd_dev;
}

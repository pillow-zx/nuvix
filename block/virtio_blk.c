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
#include <nuvix/dt.h>
#include <arch/pgtable.h>

#define VBLK_QSIZE	     8
#define VBLK_MAX_SECTORS     256u
#define VBLK_TIMEOUT_MS      30000u

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
};

struct virtio_blk_dev {
	uintptr_t mmio_base;
	uint64_t capacity;
	struct vring_desc desc[VBLK_QSIZE] __aligned(VRING_DESC_ALIGN_SIZE);
	struct vblk_avail avail __aligned(VRING_AVAIL_ALIGN_SIZE);
	struct vblk_used used __aligned(VRING_USED_ALIGN_SIZE);
	struct virtio_blk_req req;
	mutex_t submit_lock;
	spinlock_t completion_lock;
	struct wait_channel completion;
	/* Protected by completion_lock; the submit mutex owns the DMA buffers
	 * until the IRQ has consumed the used entry, even if the submitting task
	 * is killed. */
	bool pending;
	int result;
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

static const struct blkdev_ops vblk_ops = {
	.read_sectors = virtio_blk_read_sectors,
	.write_sectors = virtio_blk_write_sectors,
};

static struct virtio_blk_dev vblk = {
	.submit_lock = MUTEX_INIT(vblk.submit_lock),
	.completion_lock = SPINLOCK_INIT,
	.completion = WAIT_CHANNEL_INIT(vblk.completion),
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

	virtio_mmio_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
	if (!(virtio_mmio_read(base, VIRTIO_MMIO_DEVICE_FEATURES) &
	      (1u << (VIRTIO_F_VERSION_1 - 32))))
		panic("virtio-blk: VERSION_1 is not offered");
	virtio_mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
	virtio_mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);

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

static void vblk_build_req(struct virtio_blk_dev *dev, uintptr_t buf_addr,
			   uint64_t sector, uint32_t nsec, bool write)
{
	dev->req.hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	dev->req.hdr.ioprio = 0;
	dev->req.hdr.sector = sector;
	dev->req.status = 0xff;

	dev->desc[0].addr = __pa(&dev->req.hdr);
	dev->desc[0].len = sizeof(dev->req.hdr);
	dev->desc[0].flags = VRING_DESC_F_NEXT;
	dev->desc[0].next = 1;

	dev->desc[1].addr = __pa(buf_addr);
	dev->desc[1].len = (uint32_t)nsec * SECTOR_SIZE;
	dev->desc[1].flags =
		VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE);
	dev->desc[1].next = 2;

	dev->desc[2].addr = __pa(&dev->req.status);
	dev->desc[2].len = sizeof(dev->req.status);
	dev->desc[2].flags = VRING_DESC_F_WRITE;
	dev->desc[2].next = 0;
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
	irq_flags_t flags;
	bool completed = false;
	(void)irq;

	uint32_t status = virtio_mmio_read(base, VIRTIO_MMIO_INTERRUPT_STATUS);
	/* Acknowledge first: completions arriving during the drain can assert
	 * a fresh interrupt. Used indices, not interrupt counts, own requests. */
	virtio_mmio_write(base, VIRTIO_MMIO_INTERRUPT_ACK,
			  status & (VIRTIO_MMIO_INT_VRING | VIRTIO_MMIO_INT_CONFIG));
	if (vblk_status_get(base) & VIRTIO_CONFIG_S_NEEDS_RESET)
		panic("virtio-blk: device requires reset");
	if ((status & VIRTIO_MMIO_INT_CONFIG) && vblk_read_capacity(base) != vd->capacity)
		panic("virtio-blk: runtime capacity changes are not supported");

	spin_lock_irqsave(&vd->completion_lock, flags);
	uint16_t used = vd->used.idx;
	virtio_rmb();
	if (used != vd->last_used) {
		struct vring_used_elem *entry =
			&vd->used.ring[vd->last_used % VBLK_QSIZE];
		if (!vd->pending || (uint16_t)(used - vd->last_used) != 1 ||
		    entry->id != 0 || used != vd->avail.idx)
			panic("virtio-blk: invalid used ring completion");
		vd->last_used = used;
		vd->result = vd->req.status == VIRTIO_BLK_S_OK ? 0 : -EIO;
		vd->pending = false;
		completed = true;
	}
	spin_unlock_irqrestore(&vd->completion_lock, flags);
	if (completed)
		wait_channel_wake_all(&vd->completion);
}

static int vblk_submit_and_wait(struct virtio_blk_dev *dev)
{
	vaddr_t base = dev->mmio_base;
	struct wait_deadline deadline;
	bool submitted = false;
	int ret = mtime_deadline_from_ms(VBLK_TIMEOUT_MS, &deadline);
	if (ret)
		return ret;

	for (;;) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		irq_flags_t flags;
		ret = wait_scope_begin(&scope, 0, &deadline);
		if (ret) {
			BUG_ON(submitted);
			return ret;
		}
		spin_lock_irqsave(&dev->completion_lock, flags);
		if (submitted && !dev->pending) {
			ret = dev->result;
			spin_unlock_irqrestore(&dev->completion_lock, flags);
			return ret;
		}
		ret = wait_scope_prepare(&scope, &dev->completion, false);
		if (ret) {
			spin_unlock_irqrestore(&dev->completion_lock, flags);
			BUG_ON(submitted);
			return ret;
		}
		if (!submitted) {
			BUG_ON(dev->pending);
			dev->pending = true;
			dev->avail.ring[dev->avail.idx % VBLK_QSIZE] = 0;
			virtio_wmb();
			dev->avail.idx = (uint16_t)(dev->avail.idx + 1);
			/* The MMIO fence publishes DMA memory before notification. */
			virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
			submitted = true;
		}
		spin_unlock_irqrestore(&dev->completion_lock, flags);
		ret = wait_scope_block(&scope, &outcome);
		BUG_ON(ret < 0);
		if (outcome == WAIT_OUTCOME_TIMEOUT) {
			spin_lock_irqsave(&dev->completion_lock, flags);
			bool pending = dev->pending;
			spin_unlock_irqrestore(&dev->completion_lock, flags);
			/* Do not return a DMA buffer to its owner while the device
			 * may still write it. Preserve the fatal-stall policy. */
			if (pending)
				panic("virtio-blk: request timed out after %u ms", VBLK_TIMEOUT_MS);
		}
	}
}

static int virtio_blk_rw(struct blkdev *bdev, bool write, uintptr_t buf_addr,
			 uint64_t sector, uint32_t nsec)
{
	struct virtio_blk_dev *vd = bdev->bd_private;
	int ret;

	if (nsec == 0 || nsec > VBLK_MAX_SECTORS)
		return -EINVAL;
	if (nsec > vd->capacity || sector > vd->capacity - nsec)
		return -EINVAL;

	if (!wait_may_block())
		return -EINVAL;
	mutex_lock(&vd->submit_lock);
	vblk_build_req(vd, buf_addr, sector, nsec, write);
	ret = vblk_submit_and_wait(vd);
	mutex_unlock(&vd->submit_lock);
	return ret;
}

static int virtio_blk_read_sectors(struct blkdev *bdev, void *buf,
				   uint64_t sector, uint32_t nsec)
{
	return virtio_blk_rw(bdev, false, (uintptr_t)buf, sector, nsec);
}

static int virtio_blk_write_sectors(struct blkdev *bdev, const void *buf,
				    uint64_t sector, uint32_t nsec)
{
	return virtio_blk_rw(bdev, true, (uintptr_t)buf, sector, nsec);
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

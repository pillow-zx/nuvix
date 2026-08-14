/*
 * block/virtio_blk.c - virtio-blk MMIO 驱动（modern 传输层，轮询模式）
 */

#include <drivers/virtio_blk.h>
#include <drivers/virtio.h>
#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/tools.h>
#include <nuvix/page.h>

#define VBLK_QSIZE	     8
#define VBLK_MAX_SECTORS     256u
#define VBLK_POLL_SPIN_LIMIT 100000000u

struct vblk_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VBLK_QSIZE];
	uint16_t used_event;
};

struct vblk_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[VBLK_QSIZE];
	uint16_t avail_event;
};

struct virtio_blk_req {
	struct virtio_blk_outhdr hdr;
	uint8_t status;
};

struct virtio_blk_dev {
	uintptr_t mmio_base;
	uint64_t capacity;
};

static void vblk_status_set(vaddr_t base, uint32_t bits)
{
	virtio_mmio_write(base, VIRTIO_MMIO_STATUS, bits);
}

static uint32_t vblk_status_get(vaddr_t base)
{
	return virtio_mmio_read(base, VIRTIO_MMIO_STATUS);
}

static struct vring_desc vblk_desc[VBLK_QSIZE] __aligned(VRING_DESC_ALIGN_SIZE);
static struct vblk_avail vblk_avail __aligned(VRING_AVAIL_ALIGN_SIZE);
static struct vblk_used vblk_used __aligned(VRING_USED_ALIGN_SIZE);
static struct virtio_blk_req vblk_req;
static struct virtio_blk_dev vblk_dev;

static int virtio_blk_read_sectors(struct blkdev *bdev, void *buf,
				   uint64_t sector, uint32_t nsec);
static int virtio_blk_write_sectors(struct blkdev *bdev, const void *buf,
				    uint64_t sector, uint32_t nsec);

static const struct blkdev_ops vblk_ops = {
	.read_sectors = virtio_blk_read_sectors,
	.write_sectors = virtio_blk_write_sectors,
};

static struct blkdev vblk_bdev = {
	.bd_dev = MKDEV(VIRTIO_BLK_MAJOR, 0),
	.bd_ops = &vblk_ops,
	.bd_private = &vblk_dev,
};

static void vblk_setup_queue(uintptr_t base)
{
	uint32_t qnum_max;

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_SEL, 0);

	qnum_max = virtio_mmio_read(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qnum_max < VBLK_QSIZE)
		panic("virtio-blk: queue too small (max=%u, need=%u)\n",
		      qnum_max, VBLK_QSIZE);

	memset(&vblk_desc, 0, sizeof(vblk_desc));
	memset(&vblk_avail, 0, sizeof(vblk_avail));
	memset(&vblk_used, 0, sizeof(vblk_used));

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_NUM, VBLK_QSIZE);

	virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_DESC_LOW, __pa(vblk_desc));
	virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW,
			    __pa(&vblk_avail));
	virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_USED_LOW, __pa(&vblk_used));

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_READY, 1);
}

static void vblk_negotiate_features(vaddr_t base)
{
	uint32_t status;

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

static void vblk_build_req(uintptr_t buf_addr, uint64_t sector, uint32_t nsec,
			   bool write)
{
	vblk_req.hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	vblk_req.hdr.ioprio = 0;
	vblk_req.hdr.sector = sector;
	vblk_req.status = 0xff;

	vblk_desc[0].addr = __pa(&vblk_req.hdr);
	vblk_desc[0].len = sizeof(vblk_req.hdr);
	vblk_desc[0].flags = VRING_DESC_F_NEXT;
	vblk_desc[0].next = 1;

	vblk_desc[1].addr = __pa(buf_addr);
	vblk_desc[1].len = (uint32_t)nsec * SECTOR_SIZE;
	vblk_desc[1].flags =
		VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE);
	vblk_desc[1].next = 2;

	vblk_desc[2].addr = __pa(&vblk_req.status);
	vblk_desc[2].len = sizeof(vblk_req.status);
	vblk_desc[2].flags = VRING_DESC_F_WRITE;
	vblk_desc[2].next = 0;
}

static int vblk_submit_and_wait(vaddr_t base, uint16_t expected)
{
	volatile uint16_t *used_idx = (volatile uint16_t *)&vblk_used.idx;
	uint32_t spins = 0;

	vblk_avail.ring[vblk_avail.idx % VBLK_QSIZE] = 0;
	virtio_wmb();
	vblk_avail.idx = expected;

	virtio_mmio_write(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

	while (*used_idx != expected) {
		if (++spins > VBLK_POLL_SPIN_LIMIT)
			panic("virtio-blk: device stalled "
			      "(used->idx=%u, expected=%u)\n",
			      (unsigned int)*used_idx, (unsigned int)expected);
	}
	virtio_rmb();

	if (vblk_req.status != VIRTIO_BLK_S_OK)
		return -EIO;

	return 0;
}

static int virtio_blk_rw(struct blkdev *bdev, bool write, uintptr_t buf_addr,
			 uint64_t sector, uint32_t nsec)
{
	struct virtio_blk_dev *vd = bdev->bd_private;
	uint16_t expected;

	if (nsec == 0 || nsec > VBLK_MAX_SECTORS)
		return -EINVAL;
	if (nsec > vd->capacity || sector > vd->capacity - nsec)
		return -EINVAL;

	vblk_build_req(buf_addr, sector, nsec, write);
	expected = (uint16_t)(vblk_avail.idx + 1);
	return vblk_submit_and_wait(vd->mmio_base, expected);
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

void virtio_blk_init(void)
{
	uintptr_t base = VIRTIO_MMIO_BASE;
	uint32_t magic, version, cap_lo, cap_hi;
	uint32_t status;

	magic = virtio_mmio_read(base, VIRTIO_MMIO_MAGIC_VALUE);
	if (magic != VIRTIO_MMIO_MAGIC)
		panic("virtio-blk: bad magic 0x%x at 0x%lx (expected 0x%x)\n",
		      magic, base, VIRTIO_MMIO_MAGIC);

	version = virtio_mmio_read(base, VIRTIO_MMIO_VERSION);
	if (version != 2)
		panic("virtio-blk: unsupported transport version %u (need "
		      "modern=2)\n",
		      version);

	vblk_status_set(base, 0);

	vblk_status_set(base, VIRTIO_CONFIG_S_ACKNOWLEDGE);

	vblk_status_set(base,
			VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

	vblk_negotiate_features(base);

	status = VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
		 VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK;
	vblk_status_set(base, status);

	vblk_setup_queue(base);

	cap_lo = virtio_mmio_read(base, VIRTIO_MMIO_CONFIG);
	cap_hi = virtio_mmio_read(base, VIRTIO_MMIO_CONFIG + 4);
	vblk_dev.mmio_base = base;
	vblk_dev.capacity = (uint64_t)cap_lo | ((uint64_t)cap_hi << 32);

	vblk_bdev.bd_sectors = vblk_dev.capacity;

	register_blkdev(&vblk_bdev);

	pr_info("virtio_blk: init ok, capacity=%llu sectors (%llu MB)\n",
		(unsigned long long)vblk_dev.capacity,
		(unsigned long long)(vblk_dev.capacity >> 11));
}

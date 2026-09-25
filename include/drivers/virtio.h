/**
 * @file virtio.h
 * @brief virtio MMIO register offsets, status bits, and vring ABI structs.
 */

#ifndef _NUVIX_DRIVERS_VIRTIO_H
#define _NUVIX_DRIVERS_VIRTIO_H

#include <nuvix/config.h>
#include <nuvix/types.h>
#include <nuvix/tools.h>
#include <nuvix/barrier.h>
#include <nuvix/mmio.h>

#define VIRTIO_MMIO_MAGIC		0x74726976u
#define VIRTIO_MMIO_MAGIC_VALUE		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_INT_VRING		0x01u
#define VIRTIO_MMIO_INT_CONFIG		0x02u
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc
#define VIRTIO_MMIO_CONFIG		0x100

#define VIRTIO_CONFIG_S_ACKNOWLEDGE     0x01
#define VIRTIO_CONFIG_S_DRIVER	        0x02
#define VIRTIO_CONFIG_S_DRIVER_OK       0x04
#define VIRTIO_CONFIG_S_FEATURES_OK     0x08
#define VIRTIO_CONFIG_S_NEEDS_RESET     0x40
#define VIRTIO_CONFIG_S_FAILED	        0x80

#define VIRTIO_F_VERSION_1	        32u

#define VRING_DESC_F_NEXT               0x01
#define VRING_DESC_F_WRITE              0x02
#define VRING_DESC_F_INDIRECT           0x04

#define VRING_DESC_ALIGN_SIZE           16
#define VRING_AVAIL_ALIGN_SIZE          2
#define VRING_USED_ALIGN_SIZE           4

/**
 * @struct vring_desc
 * @brief Virtqueue descriptor table entry defined by the virtio ABI.
 *
 * @par Fields
 * - @c addr: Guest physical address of the buffer.
 * - @c len: Buffer length in bytes.
 * - @c flags: VRING_DESC_F_* ownership/chaining bits.
 * - @c next: Next descriptor index when NEXT is set.
 */
struct vring_desc {
	paddr_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/**
 * @struct vring_used_elem
 * @brief Completion entry written by the device into the used ring.
 *
 * @par Fields
 * - @c id: Head descriptor id returned by the device.
 * - @c len: Number of bytes written by the device.
 */
struct vring_used_elem {
	uint32_t id;
	uint32_t len;
};

/**
 * @struct virtio_blk_outhdr
 * @brief Request header consumed by virtio-blk before data/status buffers.
 *
 * @par Fields
 * - @c type: VIRTIO_BLK_T_* request type.
 * - @c ioprio: I/O priority field; nuvix leaves it zero.
 * - @c sector: Starting 512-byte sector on the block device.
 */
struct virtio_blk_outhdr {
	uint32_t type;
	uint32_t ioprio;
	uint64_t sector;
};

#define VIRTIO_BLK_T_IN	        0
#define VIRTIO_BLK_T_OUT        1
#define VIRTIO_BLK_T_FLUSH      4

#define VIRTIO_BLK_F_FLUSH      9

#define VIRTIO_BLK_S_OK	        0
#define VIRTIO_BLK_S_IOERR      1
#define VIRTIO_BLK_S_UNSUPP     2

/**
 * @brief Store a 32-bit value to a virtio MMIO register.
 * @param base Mapped MMIO transport virtual address.
 * @param off Register offset.
 * @param val Value to write.
 */
static inline void virtio_mmio_write(vaddr_t base, uint32_t off, uint32_t val)
{
	mmio_mb();
	MMIO_WRITE(uint32_t, base + off, val);
	mmio_mb();
}

/**
 * @brief Load a 32-bit value from a virtio MMIO register.
 * @param base Mapped MMIO transport virtual address.
 * @param off Register offset.
 * @return Register value.
 */
__must_check
static inline uint32_t virtio_mmio_read(vaddr_t base, uint32_t off)
{
	mmio_mb();
	uint32_t value = MMIO_READ(uint32_t, base + off);
	mmio_mb();
	return value;
}

/**
 * @brief Write a 64-bit physical address to a low/high MMIO register pair.
 * @param base Mapped MMIO transport virtual address.
 * @param low_off Offset of the low 32-bit register.
 * @param val 64-bit value to split little-word order.
 */
static inline void virtio_mmio_write64(vaddr_t base, uint32_t low_off,
				       uint64_t val)
{
	virtio_mmio_write(base, low_off, (uint32_t)val);
	virtio_mmio_write(base, low_off + 4, (uint32_t)(val >> 32));
}

static inline void virtio_mb(void)
{
	arch_mb();
}

static inline void virtio_wmb(void)
{
	arch_wmb();
}

static inline void virtio_rmb(void)
{
	arch_rmb();
}

#endif

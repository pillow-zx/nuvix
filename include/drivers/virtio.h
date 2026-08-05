/**
 * @file virtio.h
 * @brief virtio MMIO register offsets, status bits, and vring ABI structs.
 */

#ifndef _CUTEOS_DRIVERS_VIRTIO_H
#define _CUTEOS_DRIVERS_VIRTIO_H

#include <kernel/types.h>
#include <kernel/tools.h>

/**
 * @def VIRTIO_MMIO_BASE
 * @brief QEMU virt base address of the first virtio MMIO transport.
 */
#define VIRTIO_MMIO_BASE 0x10001000UL

/**
 * @def VIRTIO_MMIO_MAGIC
 * @brief Little-endian "virt" magic value exposed by virtio MMIO devices.
 */
#define VIRTIO_MMIO_MAGIC 0x74726976u

/** @def VIRTIO_MMIO_MAGIC_VALUE Magic register offset. */
#define VIRTIO_MMIO_MAGIC_VALUE		0x000
/** @def VIRTIO_MMIO_VERSION Transport version register offset. */
#define VIRTIO_MMIO_VERSION		0x004
/** @def VIRTIO_MMIO_DEVICE_ID Device type register offset. */
#define VIRTIO_MMIO_DEVICE_ID		0x008
/** @def VIRTIO_MMIO_VENDOR_ID Vendor id register offset. */
#define VIRTIO_MMIO_VENDOR_ID		0x00c
/** @def VIRTIO_MMIO_DEVICE_FEATURES Device feature bits register offset. */
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
/** @def VIRTIO_MMIO_DEVICE_FEATURES_SEL Device feature word selector. */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
/** @def VIRTIO_MMIO_DRIVER_FEATURES Driver-accepted feature bits. */
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
/** @def VIRTIO_MMIO_DRIVER_FEATURES_SEL Driver feature word selector. */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
/** @def VIRTIO_MMIO_QUEUE_SEL Virtqueue selector register offset. */
#define VIRTIO_MMIO_QUEUE_SEL		0x030
/** @def VIRTIO_MMIO_QUEUE_NUM_MAX Maximum size of selected virtqueue. */
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
/** @def VIRTIO_MMIO_QUEUE_NUM Driver-selected virtqueue size. */
#define VIRTIO_MMIO_QUEUE_NUM		0x038
/** @def VIRTIO_MMIO_QUEUE_READY Selected virtqueue ready flag. */
#define VIRTIO_MMIO_QUEUE_READY		0x044
/** @def VIRTIO_MMIO_QUEUE_NOTIFY Queue notification register offset. */
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
/** @def VIRTIO_MMIO_INTERRUPT_STATUS Pending interrupt status bits. */
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
/** @def VIRTIO_MMIO_INTERRUPT_ACK Interrupt acknowledge register. */
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
/** @def VIRTIO_MMIO_STATUS Device status register offset. */
#define VIRTIO_MMIO_STATUS		0x070
/** @def VIRTIO_MMIO_QUEUE_DESC_LOW Descriptor table low 32 address bits. */
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
/** @def VIRTIO_MMIO_QUEUE_DESC_HIGH Descriptor table high 32 address bits. */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
/** @def VIRTIO_MMIO_QUEUE_AVAIL_LOW Available ring low 32 address bits. */
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
/** @def VIRTIO_MMIO_QUEUE_AVAIL_HIGH Available ring high 32 address bits. */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094
/** @def VIRTIO_MMIO_QUEUE_USED_LOW Used ring low 32 address bits. */
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
/** @def VIRTIO_MMIO_QUEUE_USED_HIGH Used ring high 32 address bits. */
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4
/** @def VIRTIO_MMIO_CONFIG_GENERATION Config generation register offset. */
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc
/** @def VIRTIO_MMIO_CONFIG Device-specific configuration base offset. */
#define VIRTIO_MMIO_CONFIG		0x100

/** @def VIRTIO_CONFIG_S_ACKNOWLEDGE Driver noticed the device. */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 0x01
/** @def VIRTIO_CONFIG_S_DRIVER Driver knows how to drive the device. */
#define VIRTIO_CONFIG_S_DRIVER	    0x02
/** @def VIRTIO_CONFIG_S_DRIVER_OK Driver setup is complete. */
#define VIRTIO_CONFIG_S_DRIVER_OK   0x04
/** @def VIRTIO_CONFIG_S_FEATURES_OK Feature negotiation succeeded. */
#define VIRTIO_CONFIG_S_FEATURES_OK 0x08
/** @def VIRTIO_CONFIG_S_NEEDS_RESET Device requires reset. */
#define VIRTIO_CONFIG_S_NEEDS_RESET 0x40
/** @def VIRTIO_CONFIG_S_FAILED Driver setup failed. */
#define VIRTIO_CONFIG_S_FAILED	    0x80

/**
 * @def VIRTIO_F_VERSION_1
 * @brief Feature bit selecting modern virtio 1.x device semantics.
 */
#define VIRTIO_F_VERSION_1 32u

/** @def VRING_DESC_F_NEXT Descriptor is chained through next. */
#define VRING_DESC_F_NEXT     0x01
/** @def VRING_DESC_F_WRITE Device writes into the described buffer. */
#define VRING_DESC_F_WRITE    0x02
/** @def VRING_DESC_F_INDIRECT Descriptor points to an indirect table. */
#define VRING_DESC_F_INDIRECT 0x04

#define VRING_DESC_ALIGN_SIZE  16
#define VRING_AVAIL_ALIGN_SIZE 2
#define VRING_USED_ALIGN_SIZE  4

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
 * - @c ioprio: I/O priority field; cuteOS leaves it zero.
 * - @c sector: Starting 512-byte sector on the block device.
 */
struct virtio_blk_outhdr {
	uint32_t type;
	uint32_t ioprio;
	uint64_t sector;
};

#define VIRTIO_BLK_T_IN	 0
#define VIRTIO_BLK_T_OUT 1

#define VIRTIO_BLK_S_OK	    0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/**
 * @brief Store a 32-bit value to a virtio MMIO register.
 * @param base MMIO transport base physical address.
 * @param off Register offset.
 * @param val Value to write.
 */
__always_inline
static inline void virtio_mmio_write(paddr_t base, uint32_t off, uint32_t val)
{
	MMIO_WRITE(uint32_t, base + off, val);
}

/**
 * @brief Load a 32-bit value from a virtio MMIO register.
 * @param base MMIO transport base physical address.
 * @param off Register offset.
 * @return Register value.
 */
__always_inline __must_check
static inline uint32_t virtio_mmio_read(paddr_t base, uint32_t off)
{
	return MMIO_READ(uint32_t, base + off);
}

/**
 * @brief Write a 64-bit physical address to a low/high MMIO register pair.
 * @param base MMIO transport base physical address.
 * @param low_off Offset of the low 32-bit register.
 * @param val 64-bit value to split little-word order.
 */
__always_inline
static inline void virtio_mmio_write64(paddr_t base, uint32_t low_off,
				       uint64_t val)
{
	virtio_mmio_write(base, low_off, (uint32_t)val);
	virtio_mmio_write(base, low_off + 4, (uint32_t)(val >> 32));
}

__always_inline
static inline void virtio_mb(void)
{
	asm volatile("fence rw,rw" ::: "memory");
}

__always_inline
static inline void virtio_wmb(void)
{
	asm volatile("fence w,w" ::: "memory");
}

__always_inline
static inline void virtio_rmb(void)
{
	asm volatile("fence r,r" ::: "memory");
}

#endif

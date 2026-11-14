obj-y += drivers/block/block.c
obj-y += drivers/char/chardev.c
obj-y += drivers/char/ns16550a.c
obj-y += drivers/char/tty.c

obj-y += drivers/device.c
obj-y += drivers/platform.c

obj-$(CONFIG_VIRTIO) += drivers/virtio/virtqueue.c
obj-$(CONFIG_VIRTIO) += drivers/virtio/virtio.c
obj-$(CONFIG_VIRTIO_BLK) += drivers/block/virtio-blk.c
obj-$(CONFIG_VIRTIO_RNG) += drivers/rng/virtio-rng.c


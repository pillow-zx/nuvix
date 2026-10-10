obj-y 																	+= drivers/block/block.c
obj-$(CONFIG_UART)											+= drivers/char/uart.c
obj-$(CONFIG_VIRTIO) 										+= drivers/virtio/virtqueue.c
obj-$(CONFIG_VIRTIO) 										+= drivers/virtio/virtio.c
obj-$(CONFIG_VIRTIO) 										+= drivers/virtio/virtio-mmio.c
obj-$(CONFIG_VIRTIO_BLK)								+= drivers/block/virtio-blk.c

obj-y += drivers/device.c
obj-y += drivers/platform.c

obj-y 																	+= drivers/block.c
obj-$(CONFIG_UART)											+= drivers/uart.c
obj-$(CONFIG_VIRTIO) 										+= drivers/virtio/virtqueue.c
obj-$(CONFIG_VIRTIO) 										+= drivers/virtio/virtio.c
obj-$(CONFIG_VIRTIO) 										+= drivers/virtio/virtio-mmio.c
obj-$(CONFIG_VIRTIO_BLK)								+= drivers/virtio-blk.c

obj-y += drivers/device.c
obj-y += drivers/platform.c
obj-y += drivers/init.c

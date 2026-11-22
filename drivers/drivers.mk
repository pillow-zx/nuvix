obj-y += drivers/block/block.c
obj-y += drivers/block/loop.c
obj-y += drivers/char/chardev.c
obj-y += drivers/char/mem.c
obj-y += drivers/char/ns16550a.c
obj-y += drivers/char/tty.c

obj-y += drivers/device.c
obj-y += drivers/platform.c

obj-$(CONFIG_RTC_DEVICE) += drivers/rtc/goldfish.c

obj-$(CONFIG_VIRTIO) += drivers/virtio/virtqueue.c
obj-$(CONFIG_VIRTIO) += drivers/virtio/virtio.c
obj-$(CONFIG_VIRTIO_BLK) += drivers/block/virtblk.c
obj-$(CONFIG_VIRTIO_RNG) += drivers/rng/virtrng.c
obj-$(CONFIG_VIRTIO_SND) += drivers/sound/virtsnd.c
obj-$(CONFIG_VIRTIO_GPU) += drivers/gpu/virtgpu.c
obj-$(CONFIG_VIRTIO_INPUT) += drivers/input/virtinput.c


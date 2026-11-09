#include <nuvix/device.h>
#include <nuvix/platform.h>
#include <nuvix/tty.h>
#include <nuvix/virtio.h>

int device_init(const struct dtb *dtb)
{
    int ret = platform_scan(dtb);

    if (ret)
        return ret;
#ifdef CONFIG_VIRTIO
    return virtio_mmio_scan(dtb);
#else
    return 0;
#endif
}

int driver_init(void)
{
    int ret = ns16550a_register();

    if (ret)
        return ret;
#ifdef CONFIG_VIRTIO_BLK
    ret = virtio_blk_register();
    if (ret)
        return ret;
#endif
#ifdef CONFIG_VIRTIO_RNG
    ret = virtio_rng_register();
    if (ret)
        return ret;
#endif
    return devices_probe();
}

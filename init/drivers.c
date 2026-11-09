#include <nuvix/device.h>
#include <nuvix/platform.h>
#include <nuvix/tty.h>
#include <nuvix/virtio.h>

int device_init(const struct dtb *dtb)
{
    int ret = platform_scan(dtb);

    if (ret)
        return ret;
    return virtio_mmio_scan(dtb);
}

int driver_init(void)
{
    int ret = ns16550a_register();

    if (ret)
        return ret;
    ret = virtio_blk_register();
    if (ret)
        return ret;
    ret = virtio_rng_register();
    if (ret)
        return ret;
    return devices_probe();
}

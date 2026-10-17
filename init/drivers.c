#include <nuvix/device.h>
#include <nuvix/platform.h>
#include <nuvix/ns16550a.h>
#include <nuvix/virtio.h>
#include <nuvix/macro.h>

int device_init(const struct dtb *dtb)
{
    int ret = platform_scan(dtb);

    if (ret)
        return ret;
    IFDEF(CONFIG_VIRTIO, {
        ret = virtio_mmio_scan(dtb);
        if (ret)
            return ret;
    });
    return 0;
}

int driver_init(void)
{
    IFDEF(CONFIG_NS16550A, {
        int ret = ns16550a_register();

        if (ret)
            return ret;
    });
    IFDEF(CONFIG_VIRTIO_BLK, {
        int ret = virtio_blk_register();

        if (ret)
            return ret;
    });
    return devices_probe();
}

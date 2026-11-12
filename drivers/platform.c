#include <nuvix/platform.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/libfdt.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>

static bool platform_match(const struct device *dev, const struct driver *drv)
{
    const struct platform_device *pdev = to_platform_device_const(dev);
    const struct platform_driver *pdrv = to_platform_driver(drv);

    for (size_t pos = 0; pos < pdev->compat_len;) {
        const char *compat = pdev->compat + pos;
        size_t len = strnlen(compat, pdev->compat_len - pos);

        for (const struct of_device_id *id = pdrv->ids; id->compatible; ++id)
            if (strlen(id->compatible) == len &&
                !memcmp(compat, id->compatible, len))
                return true;
        pos += len + 1;
    }
    return false;
}

static int platform_probe(struct device *dev)
{
    return dev->drv->probe(dev);
}

const struct bus platform_bus = {
    .name = "platform",
    .match = platform_match,
    .probe = platform_probe,
};

const struct resource *platform_get_resource(const struct platform_device *dev,
                                             enum resource_type type,
                                             unsigned int index)
{
    for (unsigned int i = 0; i < dev->nres; ++i) {
        if (dev->res[i].type != type)
            continue;
        if (!index)
            return &dev->res[i];
        --index;
    }
    return NULL;
}

int platform_get_u32(const struct platform_device *dev, const char *property,
                     uint32_t *value)
{
    const fdt32_t *cells;
    int len;

    cells = fdt_getprop(dev->blob, dev->node, property, &len);
    if (!cells || len != sizeof(*cells))
        return -EINVAL;
    *value = fdt32_to_cpu(*cells);
    return 0;
}

static int platform_mem_resource(const struct dtb_range *range, void *arg)
{
    struct platform_device *dev = arg;

    if (dev->nres == PLATFORM_MAX_RESOURCES)
        return -E2BIG;
    dev->res[dev->nres++] = (struct resource){
        .type = RES_MEM,
        .start = range->base,
        .size = range->size,
    };
    return 0;
}

int platform_scan(void)
{
    const void *blob = dtb.blob;
    int node = -1, ret;

    while ((node = fdt_node_offset_by_compatible(blob, node, "ns16550a")) >=
           0) {
        struct platform_device *dev __free(kfree) = NULL;
        const char *compat;
        unsigned int irq;
        int len;

        ret = dtb_node_available(node);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        compat = fdt_getprop(blob, node, "compatible", &len);
        if (!compat || len <= 0)
            return -EINVAL;
        for (size_t pos = 0; pos < (size_t)len;) {
            size_t length = strnlen(compat + pos, (size_t)len - pos);

            if (!length || length == (size_t)len - pos)
                return -EINVAL;
            pos += length + 1;
        }
        dev = kzalloc(sizeof(*dev));
        if (!dev)
            return -ENOMEM;
        dev->dev.name = fdt_get_name(blob, node, NULL);
        dev->dev.bus = &platform_bus;
        dev->blob = blob;
        dev->node = node;
        dev->compat = compat;
        dev->compat_len = len;
        ret = dtb_scan_reg(node, platform_mem_resource, dev);
        if (ret)
            return ret;
        if (dev->nres == PLATFORM_MAX_RESOURCES)
            return -E2BIG;
        ret = irq_parse(node, &irq);
        if (ret)
            return ret;
        dev->res[dev->nres++] = (struct resource){
            .type = RES_IRQ,
            .start = irq,
            .size = 1,
        };
        ret = device_register(&dev->dev);
        if (ret)
            return ret;
        keep(dev);
    }
    return node == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
}

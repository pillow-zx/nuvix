#ifndef _NUVIX_PLATFORM_H
#define _NUVIX_PLATFORM_H

#include <nuvix/device.h>

enum resource_type {
    RES_MEM,
    RES_IRQ,
};

struct resource {
    enum resource_type type;
    /* CPU physical address and byte length, or parsed IRQ number and count. */
    uint64_t start;
    uint64_t size;
};

#define PLATFORM_MAX_RESOURCES 4

struct platform_device {
    struct device dev;
    const void *blob;
    int node;
    /* Validated NUL-separated strings borrowed from the permanent DTB. */
    const char *compat;
    size_t compat_len;
    struct resource res[PLATFORM_MAX_RESOURCES];
    unsigned int nres;
};

struct of_device_id {
    const char *compatible;
};

struct platform_driver {
    struct driver drv;
    const struct of_device_id *ids;
};

static inline struct platform_device *to_platform_device(struct device *dev)
{
    return container_of(dev, struct platform_device, dev);
}

static inline const struct platform_device *
to_platform_device_const(const struct device *dev)
{
    return container_of(dev, struct platform_device, dev);
}

static inline const struct platform_driver *
to_platform_driver(const struct driver *drv)
{
    return container_of(drv, struct platform_driver, drv);
}

extern const struct bus platform_bus;

const struct resource *platform_get_resource(const struct platform_device *dev,
                                             enum resource_type type,
                                             unsigned int index);
int platform_get_u32(const struct platform_device *dev, const char *property,
                     uint32_t *value);
int platform_scan(const struct dtb *dtb);

#endif /* _NUVIX_PLATFORM_H */

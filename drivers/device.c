#include <nuvix/device.h>
#include <nuvix/module.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>

static LIST_HEAD(devices);
static LIST_HEAD(drivers);
static bool probe_started;

static bool bus_valid(const struct bus *bus)
{
    return bus && bus->name && *bus->name && bus->match && bus->probe;
}

int device_register(struct device *dev)
{
    struct device *entry;

    Assert(local_irqs_disabled(), "device registration requires IRQs off\n");
    if (probe_started)
        return -EBUSY;
    if (!dev || !dev->name || !*dev->name || !bus_valid(dev->bus))
        return -EINVAL;
    list_for_each_entry (entry, &devices, node)
        if (entry == dev)
            return -EEXIST;
    dev->state = DEV_UNBOUND;
    dev->drv = NULL;
    dev->drvdata = NULL;
    list_add_tail(&dev->node, &devices);
    return 0;
}

int driver_register(struct driver *drv)
{
    struct driver *entry;

    Assert(local_irqs_disabled(), "driver registration requires IRQs off\n");
    if (probe_started)
        return -EBUSY;
    if (!drv || !drv->name || !*drv->name || !bus_valid(drv->bus) ||
        !drv->probe)
        return -EINVAL;
    list_for_each_entry (entry, &drivers, node)
        if (entry == drv)
            return -EEXIST;
    list_add_tail(&drv->node, &drivers);
    return 0;
}

int devices_init(void)
{
    struct device *dev;

    Assert(local_irqs_disabled(), "device probing requires IRQs off\n");
    if (probe_started)
        return -EBUSY;
    probe_started = true;
    list_for_each_entry (dev, &devices, node) {
        const struct driver *matched = NULL;
        struct driver *drv;
        int ret;

        list_for_each_entry (drv, &drivers, node) {
            if (dev->bus != drv->bus || !dev->bus->match(dev, drv))
                continue;
            matched = drv;
            break;
        }
        if (!matched) {
            pr_info("device %s (%s): no matching driver\n", dev->name,
                    dev->bus->name);
            continue;
        }
        dev->drv = matched;
        ret = dev->bus->probe(dev);
        if (ret) {
            dev->drvdata = NULL;
            dev->drv = NULL;
            pr_err("device %s (%s): driver %s probe failed (%d)\n", dev->name,
                   dev->bus->name, matched->name, ret);
            return ret;
        }
        dev->state = DEV_BOUND;
        pr_info("device %s (%s): bound to %s\n", dev->name, dev->bus->name,
                matched->name);
    }
    return 0;
}

MODULE_INIT(devices_init, 85);

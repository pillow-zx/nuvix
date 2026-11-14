#include <nuvix/device.h>
#include <nuvix/module.h>
#include <nuvix/assert.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>

static LIST_HEAD(devices);
static LIST_HEAD(drivers);

void device_register(struct device *dev)
{
    Assert(local_irqs_disabled(), "device registration requires IRQs off\n");
    dev->state = DEV_UNBOUND;
    dev->drv = NULL;
    dev->drvdata = NULL;
    list_add_tail(&dev->node, &devices);
}

void driver_register(struct driver *drv)
{
    Assert(local_irqs_disabled(), "driver registration requires IRQs off\n");
    list_add_tail(&drv->node, &drivers);
}

static int devices_init(void)
{
    struct device *dev;

    Assert(local_irqs_disabled(), "device probing requires IRQs off\n");
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

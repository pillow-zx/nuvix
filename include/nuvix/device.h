#ifndef _NUVIX_DEVICE_H
#define _NUVIX_DEVICE_H

#include <nuvix/compiler.h>
#include <nuvix/list.h>

struct device;
struct driver;

struct bus {
    const char *name;
    /* Identity only; no hardware access or resource allocation. */
    bool (*match)(const struct device *dev, const struct driver *drv);
    int (*probe)(struct device *dev);
};

enum dev_state {
    DEV_UNBOUND,
    DEV_BOUND,
};

struct device {
    struct list_head node;
    const char *name;
    const struct bus *bus;
    enum dev_state state;
    /* Selected before probe; retained on success and cleared on failure. */
    const struct driver *drv;
    void *drvdata;
};

struct driver {
    struct list_head node;
    const char *name;
    const struct bus *bus;
    int (*probe)(struct device *dev);
};

/*
 * Boot-only, before SMP, with local IRQs off. Registration only records objects;
 * objects and all referenced descriptions live for the kernel lifetime.
 */
void device_register(struct device *dev);
void driver_register(struct driver *drv);

#endif /* _NUVIX_DEVICE_H */

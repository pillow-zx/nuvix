#ifndef _NUVIX_DEVICE_H
#define _NUVIX_DEVICE_H

#include <nuvix/compiler.h>
#include <nuvix/list.h>

/* Shared ext2-new/stat layout: minor[19:8] | major[11:0] | minor[7:0]. */
#define MAJOR(dev) ((unsigned int)(((dev) >> 8) & 0xfff))
#define MINOR(dev) ((unsigned int)(((dev) & 0xff) | (((dev) >> 12) & 0xfff00)))
#define MKDEV(major, minor)                                                    \
    (((dev_t)(major) << 8) | ((dev_t)(minor) & 0xff) |                         \
     (((dev_t)(minor) & 0xfff00) << 12))

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

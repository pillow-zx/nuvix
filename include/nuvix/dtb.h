#ifndef _NUVIX_DTB_H
#define _NUVIX_DTB_H

#include <nuvix/ctypes.h>

struct dtb {
    const void *blob;
    uint64_t paddr;
    uint32_t size;
};

struct dtb_range {
    uint64_t base;
    uint64_t size;
    bool no_map;
};

typedef int (*dtb_range_fn)(const struct dtb_range *range, void *arg);

int dtb_init(struct dtb *dtb, const void *blob, paddr_t paddr);
int dtb_get_string(const struct dtb *dtb, const char *path,
                   const char *property, const char **value);
int dtb_get_u32(const struct dtb *dtb, const char *path, const char *property,
                uint32_t *value);
int dtb_scan_memory(const struct dtb *dtb, dtb_range_fn visit, void *arg);
int dtb_scan_reserved(const struct dtb *dtb, dtb_range_fn visit, void *arg);

#endif /* _NUVIX_DTB_H */

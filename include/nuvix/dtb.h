#ifndef _NUVIX_DTB_H
#define _NUVIX_DTB_H

#include <nuvix/ctypes.h>

struct dtb {
    const void *blob;
    uint64_t paddr;
    uint32_t size;
};

extern struct dtb dtb;

struct dtb_range {
    uint64_t base;
    uint64_t size;
    bool no_map;
};

typedef int (*dtb_range_fn)(const struct dtb_range *range, void *arg);
typedef int (*dtb_cpu_fn)(unsigned long hart_id, void *arg);

int dtb_init(const void *blob, paddr_t paddr);
int dtb_get_string(const char *path, const char *property, const char **value);
int dtb_get_u32(const char *path, const char *property, uint32_t *value);
/* 1 for an available node, 0 for a disabled node, or a negative error. */
int dtb_node_available(int node);
/* CPU physical ranges; currently only identity-translated peripheral buses. */
int dtb_scan_reg(int node, dtb_range_fn visit, void *arg);
int dtb_scan_memory(dtb_range_fn visit, void *arg);
int dtb_scan_reserved(dtb_range_fn visit, void *arg);
int dtb_scan_cpus(dtb_cpu_fn visit, void *arg);

#endif /* _NUVIX_DTB_H */

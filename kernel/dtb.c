#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/libfdt.h>
#include <nuvix/string.h>

static inline int fdt_errno(int error)
{
    return error == -FDT_ERR_NOTFOUND ? -ENOENT : -EINVAL;
}

static int get_string(const void *blob, int node, const char *property,
                      const char **value)
{
    int len;
    const char *string = fdt_getprop(blob, node, property, &len);

    if (!string)
        return fdt_errno(len);
    if (len <= 0 || strnlen(string, len) == (size_t)len)
        return -EINVAL;
    *value = string;
    return 0;
}

int dtb_get_string(const struct dtb *dtb, const char *path,
                   const char *property, const char **value)
{
    int node = fdt_path_offset(dtb->blob, path);

    if (node < 0)
        return fdt_errno(node);
    return get_string(dtb->blob, node, property, value);
}

int dtb_get_u32(const struct dtb *dtb, const char *path, const char *property,
                uint32_t *value)
{
    int node = fdt_path_offset(dtb->blob, path);
    int len;
    const fdt32_t *cells;

    if (node < 0)
        return fdt_errno(node);
    cells = fdt_getprop(dtb->blob, node, property, &len);
    if (!cells)
        return fdt_errno(len);
    if (len != sizeof(*cells))
        return -EINVAL;
    *value = fdt32_to_cpu(*cells);
    return 0;
}

static int node_available(const void *blob, int node)
{
    const char *status;
    int ret = get_string(blob, node, "status", &status);

    if (ret == -ENOENT)
        return 1;
    if (ret)
        return ret;
    return !strcmp(status, "okay") || !strcmp(status, "ok");
}

static uint64_t read_cells(const fdt32_t *cells, int count)
{
    uint64_t value = 0;

    for (int i = 0; i < count; ++i)
        value = (value << 32) | fdt32_to_cpu(cells[i]);
    return value;
}

static int visit_range(uint64_t base, uint64_t size, bool no_map,
                       dtb_range_fn visit, void *arg)
{
    struct dtb_range range = {.base = base, .size = size, .no_map = no_map};

    if (!size)
        return -EINVAL;
    if (base > UINT64_MAX - size)
        return -EOVERFLOW;
    return visit(&range, arg);
}

static int scan_reg(const void *blob, int node, int parent, bool no_map,
                    dtb_range_fn visit, void *arg)
{
    int address_cells = fdt_address_cells(blob, parent);
    int size_cells = fdt_size_cells(blob, parent);
    int len, stride;
    const fdt32_t *reg;

    if (address_cells < 0 || size_cells < 0)
        return -EINVAL;
    if ((address_cells != 1 && address_cells != 2) ||
        (size_cells != 1 && size_cells != 2))
        return -EOPNOTSUPP;
    stride = address_cells + size_cells;
    reg = fdt_getprop(blob, node, "reg", &len);
    if (!reg)
        return fdt_errno(len);
    if (len <= 0 || len % (stride * sizeof(*reg)))
        return -EINVAL;

    for (int i = 0; i < len / sizeof(*reg); i += stride) {
        uint64_t base = read_cells(reg + i, address_cells);
        uint64_t size = read_cells(reg + i + address_cells, size_cells);
        int ret = visit_range(base, size, no_map, visit, arg);

        if (ret)
            return ret;
    }
    return 0;
}

int dtb_scan_memory(const struct dtb *dtb, dtb_range_fn visit, void *arg)
{
    const void *blob = dtb->blob;
    int node, count = 0;

    fdt_for_each_subnode (node, blob, 0) {
        const char *type;
        int ret = get_string(blob, node, "device_type", &type);

        if (ret == -ENOENT)
            continue;
        if (ret)
            return ret;
        if (strcmp(type, "memory"))
            continue;
        ret = node_available(blob, node);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        ret = scan_reg(blob, node, 0, false, visit, arg);
        if (ret)
            return ret;
        ++count;
    }
    if (node != -FDT_ERR_NOTFOUND)
        return -EINVAL;
    return count ? 0 : -ENODEV;
}

int dtb_scan_reserved(const struct dtb *dtb, dtb_range_fn visit, void *arg)
{
    const void *blob = dtb->blob;
    int count = fdt_num_mem_rsv(blob);
    int parent, node, len, ret;

    if (count < 0)
        return -EINVAL;
    for (int i = 0; i < count; ++i) {
        uint64_t base, size;

        if (fdt_get_mem_rsv(blob, i, &base, &size))
            return -EINVAL;
        ret = visit_range(base, size, false, visit, arg);
        if (ret)
            return ret;
    }

    parent = fdt_path_offset(blob, "/reserved-memory");
    if (parent < 0)
        return parent == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
    ret = node_available(blob, parent);
    if (ret <= 0)
        return ret;
    if (!fdt_getprop(blob, parent, "ranges", &len) || len != 0)
        return -EOPNOTSUPP;
    if (fdt_address_cells(blob, parent) != fdt_address_cells(blob, 0) ||
        fdt_size_cells(blob, parent) != fdt_size_cells(blob, 0))
        return -EINVAL;

    fdt_for_each_subnode (node, blob, parent) {
        bool no_map;

        ret = node_available(blob, node);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        no_map = fdt_getprop(blob, node, "no-map", &len) != NULL;
        if ((no_map && len != 0) || (!no_map && len != -FDT_ERR_NOTFOUND))
            return -EINVAL;
        ret = scan_reg(blob, node, parent, no_map, visit, arg);
        if (ret == -ENOENT)
            return -EOPNOTSUPP;
        if (ret)
            return ret;
    }
    return node == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
}

int dtb_init(struct dtb *dtb, const void *blob, uint64_t phys_addr)
{
    uint32_t size;

    *dtb = (struct dtb){0};
    if (!blob || (phys_addr & 7))
        return -EINVAL;
    if (fdt_check_header(blob))
        return -EINVAL;
    size = fdt_totalsize(blob);
    if (phys_addr > UINT64_MAX - size)
        return -EOVERFLOW;
    if (fdt_check_full(blob, size))
        return -EINVAL;

    dtb->blob = blob;
    dtb->phys_addr = phys_addr;
    dtb->size = size;
    return 0;
}

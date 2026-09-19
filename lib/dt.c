#include <nuvix/dt.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

const void *dt_blob;

void dt_init(const void *blob, size_t size)
{
	int ret = fdt_check_full(blob, size);
	if (ret)
		panic("dt: invalid tree: %s", fdt_strerror(ret));
	dt_blob = blob;
}

const char *dt_string(int node, const char *name)
{
	int len;
	const char *s = fdt_getprop(dt_blob, node, name, &len);
	if (!s)
		return NULL;
	if (len < 1 || !memchr(s, 0, len))
		panic("dt: malformed string %s in %s", name,
		      fdt_get_name(dt_blob, node, NULL));
	return s;
}

bool dt_available(int node)
{
	for (; node >= 0; node = fdt_parent_offset(dt_blob, node)) {
		const char *s = dt_string(node, "status");
		if (s && strcmp(s, "okay") && strcmp(s, "ok"))
			return false;
	}
	return true;
}

int dt_u32(int node, const char *name, uint32_t *value)
{
	int len;
	const fdt32_t *p = fdt_getprop(dt_blob, node, name, &len);
	if (!p)
		return len == -FDT_ERR_NOTFOUND ? -ENOENT : -EINVAL;
	if (len != sizeof(*p))
		return -EINVAL;
	*value = fdt32_ld(p);
	return 0;
}

int dt_cells(const fdt32_t *p, int count, uint64_t *value)
{
	if (count < 0 || count > 2)
		return -EINVAL;
	*value = 0;
	for (int i = 0; i < count; i++)
		*value = (*value << 32) | fdt32_ld(p + i);
	return 0;
}

static bool contains(uint64_t base, uint64_t size, uint64_t addr, uint64_t len)
{
	return addr >= base && addr - base <= size && len <= size - (addr - base);
}

int dt_reg(int node, unsigned index, struct dt_resource *resource)
{
	int bus = fdt_parent_offset(dt_blob, node), len;
	int ac = fdt_address_cells(dt_blob, bus);
	int sc = fdt_size_cells(dt_blob, bus);
	const fdt32_t *p = fdt_getprop(dt_blob, node, "reg", &len);
	uint64_t addr, size;
	int stride = ac + sc;

	if (!p)
		return len == -FDT_ERR_NOTFOUND ? -ENOENT : -EINVAL;
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2 || len % (stride * 4))
		return -EINVAL;
	if (index >= (unsigned)(len / (stride * 4)))
		return -ENOENT;
	p += index * stride;
	dt_cells(p, ac, &addr);
	dt_cells(p + ac, sc, &size);
	if (!size || addr > UINT64_MAX - size)
		return -EINVAL;
	while (bus > 0) {
		int parent = fdt_parent_offset(dt_blob, bus);
		int pac = fdt_address_cells(dt_blob, parent);
		bool matched = false;
		ac = fdt_address_cells(dt_blob, bus);
		sc = fdt_size_cells(dt_blob, bus);
		p = fdt_getprop(dt_blob, bus, "ranges", &len);
		if (!p || ac < 1 || ac > 2 || pac < 1 || pac > 2 || sc < 1 || sc > 2)
			return -EINVAL;
		stride = ac + pac + sc;
		if (len % (stride * 4))
			return -EINVAL;
		if (!len)
			matched = true;
		for (int off = 0; off < len / 4; off += stride) {
			uint64_t child, target, span;
			dt_cells(p + off, ac, &child);
			dt_cells(p + off + ac, pac, &target);
			dt_cells(p + off + ac + pac, sc, &span);
			if (!contains(child, span, addr, size))
				continue;
			if (target > UINT64_MAX - (addr - child))
				return -EINVAL;
			addr = target + (addr - child);
			matched = true;
			break;
		}
		if (!matched || addr > UINT64_MAX - size)
			return -EINVAL;
		bus = parent;
	}
	*resource = (struct dt_resource){addr, size};
	return 0;
}

int dt_stdout(uint32_t *baud)
{
	int chosen = fdt_path_offset(dt_blob, "/chosen");
	const char *path = chosen >= 0 ? dt_string(chosen, "stdout-path") : NULL;
	const char *options;
	int node;
	*baud = 0;
	if (!path)
		return -ENOENT;
	options = strchr(path, ':');
	node = fdt_path_offset_namelen(dt_blob, path,
				       options ? (int)(options - path) : (int)strlen(path));
	if (node < 0 || !dt_available(node))
		panic("dt: unavailable stdout-path %s", path);
	if (options) {
		const char *p = options + 1;
		while (*p >= '0' && *p <= '9') {
			if (*baud > (UINT32_MAX - (*p - '0')) / 10)
				panic("dt: stdout baud overflow");
			*baud = *baud * 10 + (*p++ - '0');
		}
		if (!*baud || (*p && strcmp(p, "n8")))
			panic("dt: unsupported stdout options %s", options);
	}
	return node;
}

void dt_require_simple_device(int node, bool dma)
{
	static const char *const dependencies[] = {
		"clocks", "resets", "power-domains", "pinctrl-0", "iommus",
	};
	for (unsigned i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); i++)
		if (fdt_getprop(dt_blob, node, dependencies[i], NULL))
			panic("dt: %s requires unsupported %s",
			      fdt_get_name(dt_blob, node, NULL), dependencies[i]);
	if (!dma)
		return;
	for (int bus = fdt_parent_offset(dt_blob, node); bus >= 0;
	     bus = fdt_parent_offset(dt_blob, bus)) {
		int len;
		if (fdt_getprop(dt_blob, bus, "dma-ranges", &len) && len)
			panic("dt: translated DMA is not supported");
		if (fdt_getprop(dt_blob, bus, "iommu-map", NULL))
			panic("dt: IOMMU DMA is not supported");
		if (fdt_getprop(dt_blob, bus, "dma-noncoherent", NULL))
			panic("dt: noncoherent DMA is not supported");
	}
}

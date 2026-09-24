
#include <arch/page.h>
#include <arch/sbi.h>
#include <arch/boot.h>
#include <arch/plic.h>
#include <asm/trap.h>
#include <nuvix/bootmem.h>
#include <nuvix/cpu.h>
#include <nuvix/dt.h>
#include <nuvix/errno.h>
#include <nuvix/timer.h>

struct platform_match {
	const char *compatible;
};

/* Platform identity/constraints and device matching are independent. Add
 * board setup only when a supported board actually needs it. */
static const struct platform_match platforms[] = {
	IFDEF(CONFIG_PLATFORM_QEMU_VIRT, { "riscv-virtio" },)
	{ NULL },
};

static struct plic_config plic;
static int plic_node = -1;

int dt_irq(int node, unsigned index)
{
	int len, parent = -1;
	uint32_t irq;
	const fdt32_t *p;

	if (plic_node < 0 || node < 0 || !dt_available(node))
		return -ENODEV;
	p = fdt_getprop(dt_blob, node, "interrupts-extended", &len);
	if (p) {
		if (len <= 0 || len % 8)
			return -EINVAL;
		/* Only one-cell, direct PLIC specifiers are supported. */
		for (unsigned i = 0; i < (unsigned)(len / 8); i++) {
			parent = fdt_node_offset_by_phandle(dt_blob, fdt32_ld(p + i * 2));
			if (parent != plic_node)
				return -EOPNOTSUPP;
		}
		if (index >= (unsigned)(len / 8))
			return -ENOENT;
		irq = fdt32_ld(p + index * 2 + 1);
	} else {
		if (len != -FDT_ERR_NOTFOUND)
			return -EINVAL;
		for (int ancestor = node; ancestor >= 0;
		     ancestor = fdt_parent_offset(dt_blob, ancestor)) {
			uint32_t phandle;
			int ret = dt_u32(ancestor, "interrupt-parent", &phandle);
			if (!ret) {
				parent = fdt_node_offset_by_phandle(dt_blob, phandle);
				break;
			}
			if (ret != -ENOENT)
				return ret;
		}
		if (parent != plic_node)
			return -EOPNOTSUPP;
		p = fdt_getprop(dt_blob, node, "interrupts", &len);
		if (!p)
			return len == -FDT_ERR_NOTFOUND ? -ENOENT : -EINVAL;
		if (len <= 0 || len % 4)
			return -EINVAL;
		if (index >= (unsigned)(len / 4))
			return -ENOENT;
		irq = fdt32_ld(p + index);
	}
	return irq && irq <= plic.ndev ? (int)irq : -EINVAL;
}

static uint32_t plic_context_hart(int intc)
{
	int cpu = fdt_parent_offset(dt_blob, intc);
	int cpus = fdt_parent_offset(dt_blob, cpu);
	int ac = fdt_address_cells(dt_blob, cpus), len;
	const char *type = dt_string(cpu, "device_type");
	const fdt32_t *reg = fdt_getprop(dt_blob, cpu, "reg", &len);
	uint64_t hart;

	if (!type || strcmp(type, "cpu") || ac < 1 || ac > 2 ||
	    !reg || len != ac * 4 || dt_cells(reg, ac, &hart) ||
	    hart > UINT32_MAX)
		panic("plic: invalid CPU for interrupt context");
	return (uint32_t)hart;
}

static void platform_plic_discover(uint32_t boot_hartid)
{
	struct cpu_entry cpus[NR_CPUS];
	struct dt_resource extra;
	uint32_t count, cells;
	int node = -1, found = -1, len;

	if (platform_cpu_entries(boot_hartid, cpus, &count))
		panic("plic: CPU topology is unavailable");
	while ((node = fdt_next_node(dt_blob, node, NULL)) >= 0) {
		if (!dt_available(node) ||
		    (fdt_node_check_compatible(dt_blob, node, "sifive,plic-1.0.0") &&
		     fdt_node_check_compatible(dt_blob, node, "riscv,plic0")))
			continue;
		if (found >= 0)
			panic("plic: multiple controllers are not supported");
		found = node;
	}
	if (found < 0)
		panic("plic: no supported controller");
	plic_node = found;
	dt_require_simple_device(found, false);
	if (!fdt_getprop(dt_blob, found, "interrupt-controller", &len) || len ||
	    dt_u32(found, "#interrupt-cells", &cells) || cells != 1 ||
	    dt_u32(found, "#address-cells", &cells) || cells ||
	    dt_reg(found, 0, &plic.regs) ||
	    dt_reg(found, 1, &extra) != -ENOENT ||
	    (plic.regs.start & 3) ||
	    fdt_getprop(dt_blob, found, "big-endian", NULL) ||
	    dt_u32(found, "riscv,ndev", &plic.ndev) ||
	    !plic.ndev || plic.ndev > PLIC_MAX_SOURCES)
		panic("plic: invalid controller description");

	const fdt32_t *contexts =
		fdt_getprop(dt_blob, found, "interrupts-extended", &len);
	if (!contexts || len <= 0 || len % 8 ||
	    (unsigned)(len / 8) > PLIC_MAX_CONTEXTS)
		panic("plic: invalid interrupts-extended");
	for (uint32_t id = 0; id < count; id++)
		plic.contexts[id] = UINT32_MAX;

	/* Every entry occupies a hardware context, including M-mode and the
	 * -1 placeholders left by firmware for inaccessible contexts. */
	for (unsigned context = 0; context < (unsigned)(len / 8); context++) {
		uint32_t phandle = fdt32_ld(contexts + context * 2);
		uint32_t cause = fdt32_ld(contexts + context * 2 + 1);
		int intc = fdt_node_offset_by_phandle(dt_blob, phandle), size;

		if (intc < 0 ||
		    fdt_node_check_compatible(dt_blob, intc, "riscv,cpu-intc") ||
		    !fdt_getprop(dt_blob, intc, "interrupt-controller", &size) || size ||
		    dt_u32(intc, "#interrupt-cells", &cells) || cells != 1)
			panic("plic: invalid interrupt parent for context %u", context);
		if (cause == IRQ_M_EXT || cause == UINT32_MAX)
			continue;
		if (cause != IRQ_S_EXT)
			panic("plic: unsupported context interrupt %u", cause);
		if (!dt_available(intc))
			continue;
		uint32_t hart = plic_context_hart(intc);
		for (uint32_t id = 0; id < count; id++) {
			if (cpus[id].hartid != hart)
				continue;
			uint32_t logical = cpus[id].logical_id;
			if (plic.contexts[logical] != UINT32_MAX)
				panic("plic: duplicate S-mode context for hart %u", hart);
			plic.contexts[logical] = context;
		}
	}
	for (uint32_t id = 0; id < count; id++) {
		if (plic.contexts[cpus[id].logical_id] == UINT32_MAX)
			panic("plic: no S-mode context for hart %u", cpus[id].hartid);
	}
}

void platform_irq_init(void)
{
	plic_init(&plic);
}

void platform_init(uint64_t boot_hartid, paddr_t dtb_pa)
{
	struct sbi_ret ret = sbi_base_spec_version();
	bool supported = false;

	if (ret.error != 0 || ret.value < 0x20000)
		panic("sbi: version 0.2 or newer is required");
	dtb_init(dtb_pa);
	for (unsigned i = 0; platforms[i].compatible; i++) {
		if (!fdt_node_check_compatible(dt_blob, 0, platforms[i].compatible)) {
			supported = true;
			break;
		}
	}
	if (!supported)
		panic("platform: unsupported DT root compatible");
	bootmem_init();
	if (bootmem_no_map(dtb_pa, fdt_totalsize(dt_blob)))
		panic("dt: blob overlaps no-map memory");
	bootmem_reserve(dtb_pa, fdt_totalsize(dt_blob), false);
	dt_cpus_init(boot_hartid);
	platform_plic_discover((uint32_t)boot_hartid);
}

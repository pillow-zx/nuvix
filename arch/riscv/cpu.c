#include <asm/setup.h>
#include <arch/boot.h>
#include <nuvix/cpu.h>
#include <nuvix/dt.h>
#include <nuvix/errno.h>
#include <nuvix/timer.h>

static struct cpu_entry topology[NR_CPUS];
static uint32_t cpu_count;

static bool cpu_extension(int node, const char *extension)
{
	if (fdt_getprop(dt_blob, node, "riscv,isa-extensions", NULL))
		return fdt_stringlist_search(dt_blob, node, "riscv,isa-extensions",
					    extension) >= 0;

	const char *isa = dt_string(node, "riscv,isa");

	if (!isa || strncmp(isa, "rv64", 4))
		return false;

	const char *p = isa + 4;

	if (strlen(extension) == 1) {
		while (*p && *p != '_') {
			if (*p == *extension || (*p == 'g' && strchr("imafd", *extension)))
				return true;
			p++;
		}
		return false;
	}

	while ((p = strchr(p, '_'))) {
		p++;
		size_t len = strlen(extension);
		if (!strncmp(p, extension, len) &&
		    (!p[len] || p[len] == '_' || (p[len] >= '0' && p[len] <= '9')))
			return true;
	}

	return false;
}

void arch_cpu_discover(uint64_t boot_hartid)
{
	int node;
	int len;
	uint32_t freq;
	int cpus = fdt_path_offset(dt_blob, "/cpus");

	bool boot_found = false;
	bool sstc = true;

	if (cpus < 0 || boot_hartid > UINT32_MAX ||
	    dt_u32(cpus, "timebase-frequency", &freq))
		panic("cpu: missing or invalid CPU/timebase description");

	int ac = fdt_address_cells(dt_blob, cpus);
	if (ac < 1 || ac > 2 || fdt_size_cells(dt_blob, cpus) != 0)
		panic("cpu: invalid CPU cells");

	topology[0] = (struct cpu_entry){0, (uint32_t)boot_hartid};
	cpu_count = 1;

	fdt_for_each_subnode(node, dt_blob, cpus) {
		const char *type = dt_string(node, "device_type");
		if (!type || strcmp(type, "cpu") || !dt_available(node))
			continue;
		const fdt32_t *reg = fdt_getprop(dt_blob, node, "reg", &len);
		uint64_t hart;
		if (!reg || len != ac * 4 || dt_cells(reg, ac, &hart) || hart > UINT32_MAX)
			panic("cpu: invalid hart ID");
		if (hart == boot_hartid) {
			if (boot_found)
				panic("cpu: duplicate boot hart");
			boot_found = true;
		} else {
#ifndef CONFIG_SMP
			continue;
#else
			if (cpu_count == NR_CPUS)
				panic("cpu: DT exceeds CONFIG_MAX_CPUS=%u", NR_CPUS);
			for (uint32_t i = 0; i < cpu_count; i++)
				if (topology[i].hartid == hart)
					panic("cpu: duplicate hart ID");
			topology[cpu_count] = (struct cpu_entry){cpu_count, (uint32_t)hart};
			cpu_count++;
#endif
		}

		const char *mmu = dt_string(node, "mmu-type");
		const char *base = dt_string(node, "riscv,isa-base");
		const char *isa = dt_string(node, "riscv,isa");

		if ((base && strcmp(base, "rv64i")) ||
		    (!base && (!isa || strncmp(isa, "rv64", 4))))
			panic("cpu: hart %lu is not RV64", hart);

		if (!mmu || (strcmp(mmu, "riscv,sv39") && strcmp(mmu, "riscv,sv48") &&
			     strcmp(mmu, "riscv,sv57")))
			panic("cpu: hart %lu has no supported MMU", hart);

		const char *required[] = {"i", "m", "a", "f", "d", "c"};

		for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++)
			if (!cpu_extension(node, required[i]))
				panic("cpu: hart %lu lacks ISA extension %s", hart, required[i]);

		sstc &= cpu_extension(node, "sstc");
	}
	if (!boot_found)
		panic("cpu: boot hart is absent or disabled in DT");

	riscv_timer_init(freq, sstc);
}

int platform_cpu_entries(uint32_t boot_hartid,
			 struct cpu_entry *entries, uint32_t *count)
{
	if (!entries || !count || !cpu_count || topology[0].hartid != boot_hartid)
		return -EINVAL;

	memcpy(entries, topology, cpu_count * sizeof(*entries));
	*count = cpu_count;
	return 0;
}

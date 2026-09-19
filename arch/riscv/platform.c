
#include <arch/page.h>
#include <arch/sbi.h>
#include <arch/boot.h>
#include <nuvix/bootinfo.h>
#include <nuvix/bootmem.h>
#include <nuvix/cpu.h>
#include <nuvix/dt.h>
#include <nuvix/timer.h>

struct platform_match {
	const char *compatible;
	const char *name;
};

/* Platform identity/constraints and device matching are independent. Add
 * board setup only when a supported board actually needs it. */
static const struct platform_match platforms[] = {
#ifdef CONFIG_PLATFORM_QEMU_VIRT
	{ "riscv-virtio", "QEMU virt" },
#endif
	{ NULL, NULL },
};
static const struct platform_match *platform;

void platform_init(uint64_t boot_hartid, paddr_t dtb_pa)
{
	struct sbi_ret ret = sbi_base_spec_version();
	if (ret.error != 0 || ret.value < 0x20000)
		panic("sbi: version 0.2 or newer is required");
	arch_dtb_init(dtb_pa);
	for (unsigned i = 0; platforms[i].compatible; i++) {
		if (!fdt_node_check_compatible(dt_blob, 0, platforms[i].compatible)) {
			platform = &platforms[i];
			break;
		}
	}
	if (!platform)
		panic("platform: unsupported DT root compatible");
	bootmem_init();
	if (bootmem_no_map(dtb_pa, fdt_totalsize(dt_blob)))
		panic("dt: blob overlaps no-map memory");
	bootmem_reserve(dtb_pa, fdt_totalsize(dt_blob), false);
	arch_dt_cpus_init(boot_hartid);
}

extern char _start[];

BOOTINFO_BLOCK(platform, uint32_t boot_hartid,
	BROW("Platform Name", "%s", platform->name);
	BROW("CPU Capacity", "%u", (unsigned)NR_CPUS);
	BROW("Boot HART ID", "%u (logical 0)", boot_hartid);
	BROW("Boot Console", "SBI early, DT-selected UART");
	BROW("Memory Base", "0x%016llx", (unsigned long long)ram_base);
	BROW("Memory Size", "%llu MiB", (unsigned long long)(ram_size >> 20));
	BROW("Page Table Mode", "Sv39");
	BROW("Kernel Image Base", "0x%016llx",
	     (unsigned long long)__pa((uintptr_t)_start));
)

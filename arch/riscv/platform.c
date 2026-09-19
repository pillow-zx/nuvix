
#include <arch/page.h>
#include <arch/sbi.h>
#include <arch/boot.h>
#include <nuvix/bootmem.h>
#include <nuvix/cpu.h>
#include <nuvix/dt.h>
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
}


#include <arch/page.h>
#include <nuvix/bootinfo.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/smp.h>
#include <nuvix/types.h>

#define PLATFORM_QEMU_VIRT_CPUS QEMU_VIRT_MAX_CPUS

static_assert(CONFIG_QEMU_CPUS <= PLATFORM_QEMU_VIRT_CPUS,
	      "QEMU virt exposes at most eight harts");

int platform_cpu_entries(uint32_t boot_hartid,
			 struct cpu_topology_entry *entries, uint32_t *count)
{
	uint32_t logical_id = 1;

	if (!entries || !count || boot_hartid >= CONFIG_QEMU_CPUS)
		return -EINVAL;

	entries[0].logical_id = 0;
	entries[0].hartid = boot_hartid;
	for (uint32_t hartid = 0; hartid < CONFIG_QEMU_CPUS; hartid++) {
		if (hartid == boot_hartid)
			continue;
		entries[logical_id].logical_id = logical_id;
		entries[logical_id].hartid = hartid;
		logical_id++;
	}
	*count = logical_id;
	return 0;
}

extern char _start[];

BOOTINFO_BLOCK(platform, uint32_t boot_hartid,
	BROW("Platform Name", "QEMU riscv-virt");
	BROW("Platform HART Count", "%u", (unsigned)CONFIG_QEMU_CPUS);
	BROW("Boot HART ID", "%u (logical 0)", boot_hartid);
	BROW("Boot Console", "uart8250 (SBI early)");
	BROW("Memory Base", "0x%016llx", (unsigned long long)DRAM_BASE);
	BROW("Memory Size", "%llu MiB", (unsigned long long)(DRAM_SIZE >> 20));
	BROW("Page Table Mode", "Sv39");
	BROW("Kernel Image Base", "0x%016llx",
	     (unsigned long long)__pa((uintptr_t)_start));
)

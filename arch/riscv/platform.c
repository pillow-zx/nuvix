/*
 * arch/riscv/platform.c - compile-time QEMU virt CPU data
 *
 * This module owns the QEMU virt hart layout. The SBI-provided boot hart is
 * normalized to logical CPU 0, then the remaining harts are enumerated in
 * physical-hart order. Generic code consumes those {logical_id, hartid}
 * entries and never assumes they are equal.
 */

#include <arch/page.h>
#include <nuvix/bootinfo.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/smp.h>
#include <nuvix/types.h>

#define PLATFORM_QEMU_VIRT_CPUS 8

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

/*
 * Phase-A boot banner: everything here is a compile-time or link-time fact
 * (DRAM layout, kernel image symbols), so it can print before page tables and
 * the MM subsystems exist. The boot hart ID comes from the SBI boot argument,
 * since cpu_table is not populated until smp_prepare() runs.
 */
extern char _start[];

BOOTINFO_BLOCK(platform, uint32_t boot_hartid,
	BROW("Platform Name", "QEMU riscv-virt");
	BROW("Platform HART Count", "%u", (unsigned)CONFIG_QEMU_CPUS);
	/* The trailing "(logical 0)" distinguishes this row from OpenSBI's bare
	 * "Boot HART ID : N" line and documents the logical-0 normalization
	 * performed by platform_cpu_entries(). */
	BROW("Boot HART ID", "%u (logical 0)", boot_hartid);
	BROW("Boot Console", "uart8250 (SBI early)");
	BROW("Memory Base", "0x%016llx", (unsigned long long)DRAM_BASE);
	BROW("Memory Size", "%llu MiB", (unsigned long long)(DRAM_SIZE >> 20));
	BROW("Page Table Mode", "Sv39");
	BROW("Kernel Image Base", "0x%016llx",
	     (unsigned long long)__pa((uintptr_t)_start));
)

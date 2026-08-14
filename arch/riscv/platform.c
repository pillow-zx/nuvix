/*
 * arch/riscv/platform.c - compile-time QEMU virt CPU data
 *
 * This module owns the QEMU virt hart layout. The SBI-provided boot hart is
 * normalized to logical CPU 0, then the remaining harts are enumerated in
 * physical-hart order. Generic code consumes those {logical_id, hartid}
 * entries and never assumes they are equal.
 */

#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/smp.h>
#include <kernel/types.h>

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

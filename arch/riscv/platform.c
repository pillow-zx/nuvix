/*
 * arch/riscv/platform.c - compile-time QEMU virt CPU data
 *
 * This module is the sole owner of the continuous 0..7 hart layout. Generic
 * code consumes {logical_id, hartid} entries and never infers their
 * equality, and never generates hart IDs itself.
 */

#include <kernel/cpu.h>
#include <kernel/smp.h>
#include <kernel/types.h>

#define PLATFORM_QEMU_VIRT_CPUS 8

static const struct cpu_topology_entry virt_cpus[PLATFORM_QEMU_VIRT_CPUS] = {
	{ .logical_id = 0, .hartid = 0 },
	{ .logical_id = 1, .hartid = 1 },
	{ .logical_id = 2, .hartid = 2 },
	{ .logical_id = 3, .hartid = 3 },
	{ .logical_id = 4, .hartid = 4 },
	{ .logical_id = 5, .hartid = 5 },
	{ .logical_id = 6, .hartid = 6 },
	{ .logical_id = 7, .hartid = 7 },
};

static_assert(CONFIG_QEMU_CPUS <= PLATFORM_QEMU_VIRT_CPUS,
	      "QEMU virt exposes at most eight harts");

const struct cpu_topology_entry *platform_cpu_entries(uint32_t *count)
{
	if (count)
		*count = CONFIG_QEMU_CPUS;
	return virt_cpus;
}

uint32_t platform_boot_hartid(void)
{
	return 0;
}

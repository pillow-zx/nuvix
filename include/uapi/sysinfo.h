#ifndef _CUTEOS_UAPI_SYSINFO_H
#define _CUTEOS_UAPI_SYSINFO_H

/**
 * @file sysinfo.h
 * @brief Linux sysinfo UAPI result layout.
 */

/**
 * @struct sysinfo
 * @brief System summary returned by sysinfo().
 *
 * @par Fields
 * - @c uptime: Seconds since boot.
 * - @c loads: 1/5/15-minute load averages.
 * - @c totalram: Total usable memory units.
 * - @c freeram: Free memory units.
 * - @c sharedram: Shared memory units.
 * - @c bufferram: Buffer/cache memory units.
 * - @c totalswap: Total swap units.
 * - @c freeswap: Free swap units.
 * - @c procs: Current process count.
 * - @c pad: ABI padding.
 * - @c totalhigh: Total high-memory units.
 * - @c freehigh: Free high-memory units.
 * - @c mem_unit: Size in bytes of one memory unit.
 * - @c _f: Linux-compatible trailing padding marker.
 */
struct sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs;
	unsigned short pad;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;
	char _f[0];
};

_Static_assert(sizeof(struct sysinfo) == 112,
	       "Linux riscv64 sysinfo size mismatch");

#endif

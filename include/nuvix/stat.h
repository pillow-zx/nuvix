#ifndef _NUVIX_STAT_H
#define _NUVIX_STAT_H

#include <nuvix/compiler.h>
#include <uapi/stat.h>

static_assert(sizeof(struct stat) == 128,
	      "struct stat must match the riscv64 stat ABI (128 bytes)");
static_assert(offsetof(struct stat, st_mode) == 16,
	      "st_mode offset drifted from riscv64 stat ABI");
static_assert(offsetof(struct stat, st_size) == 48,
	      "st_size offset drifted from riscv64 stat ABI");
static_assert(offsetof(struct stat, st_blocks) == 64,
	      "st_blocks offset drifted from riscv64 stat ABI");
static_assert(sizeof(struct statx) == 256,
	      "struct statx must match Linux statx ABI (256 bytes)");
static_assert(offsetof(struct statx, stx_atime) == 0x40,
	      "stx_atime offset drifted from Linux statx ABI");
static_assert(offsetof(struct statx, stx_rdev_major) == 0x80,
	      "stx_rdev_major offset drifted from Linux statx ABI");

#endif

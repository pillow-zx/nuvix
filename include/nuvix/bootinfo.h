/*
 * include/nuvix/bootinfo.h - OpenSBI-style boot banner
 *
 * The banner is a sequence of blocks; each block prints a few aligned
 * "label : value" rows. The row layout lives in BROW, which calls pr_info
 * directly, so the data side is just BROW/BBLANK statements. BOOTINFO_BLOCK
 * turns a body of those statements into an emitter function, and
 * BOOTINFO_BLOCKS enumerates every block once and mints the emitter
 * declarations from that one list.
 *
 * Data stays with the subsystem that owns it: each block is defined in its
 * module, and init/main.c calls the emitters in boot order at the points
 * where their data is ready (phased banner, never collected-and-deferred).
 * Blocks with real control flow (bootinfo_sbi, bootinfo_cpu) are written by
 * hand but use the same BROW/BBLANK/appends leaves.
 */

#ifndef _NUVIX_BOOTINFO_H
#define _NUVIX_BOOTINFO_H

#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <nuvix/printk.h>

/* One aligned "label : value" row. fmt must be a string literal so the
 * label layout string-concatenates; args follow the label. */
#define BROW(label, fmt, ...)                                                  \
	pr_info("%-28s : " fmt "\n", label, ##__VA_ARGS__)

/* Blank line separating two banner blocks. */
#define BBLANK() pr_info("\n")

/*
 * Every banner block, enumerated once. Each entry mints an emitter
 * void bootinfo_<name>(args); init/main.c calls them in boot order at the
 * point where the block's data is ready. Pure-data blocks are defined with
 * BOOTINFO_BLOCK in the file that owns the data; sbi and cpu carry real
 * control flow and are written by hand.
 */
#define BOOTINFO_BLOCKS(X)                                                     \
	X(platform, uint32_t boot_hartid)                                      \
	X(sbi, void)                                                           \
	X(timer, void)                                                         \
	X(mm, void)                                                            \
	X(cpu, void)                                                           \
	X(block, const char *name)                                             \
	X(init, void)                                                          \
	X(buddy, void)                                                         \
	X(slab, void)                                                          \
	X(vmalloc, void)

#define BOOTINFO_DECL(name, ...) void bootinfo_##name(__VA_ARGS__);
BOOTINFO_BLOCKS(BOOTINFO_DECL)

/* Define a block emitter whose body is the BROW/BBLANK statements after the
 * signature, one per line, each ending in ';'. */
#define BOOTINFO_BLOCK(name, args, ...)                                        \
	void bootinfo_##name(args)                                             \
	{                                                                      \
		__VA_ARGS__                                                    \
	}

/* Bounded printf-append for chunked values (e.g. a comma-joined list):
 * formats fmt into buf + off, returns the new offset, capped at size-1. */
__printf(4, 5) __nonnull(1, 4)
static inline size_t bootinfo_append(char *buf, size_t size, size_t off,
				     const char *fmt, ...)
{
	va_list ap;
	int written;

	if (size == 0)
		return 0;
	if (off >= size)
		return size - 1;

	va_start(ap, fmt);
	written = vsnprintf(buf + off, size - off, fmt, ap);
	va_end(ap);
	if (written < 0 || (size_t)written >= size - off)
		return size - 1;
	return off + (size_t)written;
}

/* Print the version line and the project logo. */
void bootinfo_logo(void);

#endif

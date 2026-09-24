#ifndef _NUVIX_DT_H
#define _NUVIX_DT_H

#include <nuvix/libfdt.h>

struct dt_resource {
	paddr_t start;
	size_t size;
};

/* Immutable, validated blob, retained for the lifetime of the kernel. */
extern const void *dt_blob;
void dt_init(const void *blob, size_t size);
bool dt_available(int node);
const char *dt_string(int node, const char *name);
int dt_u32(int node, const char *name, uint32_t *value);
int dt_cells(const fdt32_t *cells, int count, uint64_t *value);
/* reg addresses are translated through every parent ranges property. */
int dt_reg(int node, unsigned index, struct dt_resource *resource);
int dt_stdout(uint32_t *baud);
/* Direct platform IRQ, or negative errno; no interrupt nexus translation. */
int dt_irq(int node, unsigned index);
/* Current drivers require firmware-configured clocks/reset/pins and direct DMA. */
void dt_require_simple_device(int node, bool dma);

#endif

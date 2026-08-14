#ifndef _NUVIX_KSYMS_H
#define _NUVIX_KSYMS_H

#include <nuvix/types.h>

struct ksym {
	uintptr_t addr;
	const char *name;
};

const char *ksym_lookup(uintptr_t addr, uintptr_t *offset);

#endif

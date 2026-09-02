#ifndef _NUVIX_MEMBARRIER_H
#define _NUVIX_MEMBARRIER_H

#include <nuvix/types.h>

/** Execute one Linux-compatible membarrier command for the current Task. */
ssize_t membarrier_command(int cmd, unsigned int flags, int cpu_id);

#endif

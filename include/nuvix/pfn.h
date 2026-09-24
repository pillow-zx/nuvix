#ifndef _NUVIX_PFN_H
#define _NUVIX_PFN_H

#include <nuvix/config.h>
#include <nuvix/types.h>

#define PFN_DOWN(x)    ((x) >> PAGE_SHIFT)
#define PFN_UP(x)      (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_PHYS(pfn)  ((uint64_t)(pfn) << PAGE_SHIFT)
#define PHYS_PFN(addr) ((addr) >> PAGE_SHIFT)

#endif

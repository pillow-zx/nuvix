#ifndef _NUVIX_SLEEP_H
#define _NUVIX_SLEEP_H

#include <nuvix/ctypes.h>

int sleep_until(uint64_t deadline);

int sleep_ns(uint64_t ns);

#endif /* _NUVIX_SLEEP_H */

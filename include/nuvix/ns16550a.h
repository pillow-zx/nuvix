#ifndef _NUVIX_NS16550A_H
#define _NUVIX_NS16550A_H

#include <nuvix/ctypes.h>

int ns16550a_register(void);
size_t ns16550a_read(void *buffer, size_t count);
size_t ns16550a_write(const void *buffer, size_t count);

#endif

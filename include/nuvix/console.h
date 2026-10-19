#ifndef _NUVIX_CONSOLE_H
#define _NUVIX_CONSOLE_H

struct fdtable;

int console_init(struct fdtable *fdt);

#endif

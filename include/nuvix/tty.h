#ifndef _NUVIX_TTY_H
#define _NUVIX_TTY_H

struct fdtable;

int tty_init(void);
int init_stdio(struct fdtable *fdt);

#endif

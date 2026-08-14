#ifndef _NUVIX_TTY_H
#define _NUVIX_TTY_H

#include <nuvix/types.h>

/*
 * include/nuvix/tty.h - UART-backed single-console TTY adapter
 */

void tty_console_init(void);
int tty_console_start(void) __must_check;

#endif

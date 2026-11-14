#ifndef _NUVIX_UART_H
#define _NUVIX_UART_H

#include <nuvix/ctypes.h>
#include <nuvix/tty.h>

ssize_t ns16550a_read(void *buffer, size_t count);
ssize_t ns16550a_write(const void *buffer, size_t count);
int ns16550a_set_termios(const struct termios2 *termios);
void ns16550a_flush_input(void);
int ns16550a_flush_output(void);

#endif

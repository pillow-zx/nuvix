#ifndef _NUVIX_TTY_H
#define _NUVIX_TTY_H

#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/assert.h>

struct fdtable;

#define NCCS 19

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[NCCS];
};

struct termios2 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static_assert(sizeof(struct termios) == 36, "invalid termios layout");
static_assert(sizeof(struct termios2) == 44, "invalid termios2 layout");
static_assert(sizeof(struct winsize) == 8, "invalid winsize layout");

#define ICRNL      0x00100
#define IXON       0x00400

#define OPOST      0x00001
#define ONLCR      0x00004

#define CBAUD      0x0000100f
#define B50        0x00000001
#define B75        0x00000002
#define B110       0x00000003
#define B134       0x00000004
#define B150       0x00000005
#define B200       0x00000006
#define B300       0x00000007
#define B600       0x00000008
#define B1200      0x00000009
#define B1800      0x0000000a
#define B2400      0x0000000b
#define B4800      0x0000000c
#define B9600      0x0000000d
#define B19200     0x0000000e
#define B38400     0x0000000f
#define BOTHER     0x00001000
#define B57600     0x00001001
#define B115200    0x00001002
#define B230400    0x00001003

#define CSIZE      0x00000030
#define CS5        0x00000000
#define CS6        0x00000010
#define CS7        0x00000020
#define CS8        0x00000030
#define CSTOPB     0x00000040
#define CREAD      0x00000080
#define PARENB     0x00000100
#define PARODD     0x00000200

#define ISIG       0x00001
#define ICANON     0x00002
#define ECHO       0x00008
#define ECHOE      0x00010
#define ECHOK      0x00020
#define IEXTEN     0x08000

#define VINTR      0
#define VQUIT      1
#define VERASE     2
#define VKILL      3
#define VEOF       4
#define VTIME      5
#define VMIN       6
#define VSTART     8
#define VSTOP      9
#define VSUSP      10
#define VREPRINT   12
#define VDISCARD   13
#define VWERASE    14
#define VLNEXT     15

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGPGRP  0x540f
#define TIOCSPGRP  0x5410
#define TCGETS2    0x802c542a
#define TCSETS2    0x402c542b
#define TCSETSW2   0x402c542c
#define TCSETSF2   0x402c542d
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int tty_init(void);
int init_stdio(struct fdtable *fdt);

#endif

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

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static_assert(sizeof(struct termios) == 36, "invalid termios layout");
static_assert(sizeof(struct winsize) == 8, "invalid winsize layout");

#define ICRNL      0x00100
#define IXON       0x00400

#define OPOST      0x00001
#define ONLCR      0x00004

#define CS8        0x00000030
#define CREAD      0x00000080

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
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int tty_init(void);
int init_stdio(struct fdtable *fdt);

#endif

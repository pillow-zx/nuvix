#include <nuvix/printk.h>
#include <asm/sbi.h>

static int early_getchar(void)
{
    int c;

    do {
        c = sbi_getchar();
    } while (c < 0);
    return c;
}

static int early_putchar(int c)
{
    return sbi_putchar(c);
}

int (*putchar)(int c) = early_putchar;
int (*getchar)(void) = early_getchar;

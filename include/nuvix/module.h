#ifndef _NUVIX_MODULE_H
#define _NUVIX_MODULE_H

#include <nuvix/compiler.h>
#include <nuvix/macro.h>
#include <nuvix/assert.h>

struct module_initcall {
    int (*init)(void);
    const char *name;
};

#define __MODULE_INIT(fn, order)                                               \
    static const struct module_initcall __module_##fn __used __aligned(        \
        ALIGNOF(struct module_initcall))                                       \
        __section(".init." #order "." #fn) = {                                 \
            .init = (fn),                                                      \
            .name = #fn,                                                       \
    }

/*
 * Built-in initialization after the MMU switch, on the boot CPU with IRQs off.
 * Order is a decimal literal from 0 to 999, or a macro expanding to one, with
 * no leading zeroes. Equal priorities must be independent. Priority 999 is
 * reserved for publishing the kernel initialization task.
 */
#define MODULE_INIT(fn, order)                                                 \
    static_assert((order) >= 0 && (order) <= 999,                              \
                  "module initialization order must be in [0, 999]");          \
    __MODULE_INIT(fn, order)

#define MODULE_DRIVER_INIT(name, order)                                        \
    static __always_inline int name##_init(void)                               \
    {                                                                          \
        driver_register(&name##_driver.drv);                                   \
        return 0;                                                              \
    }                                                                          \
    MODULE_INIT(name##_init, order)

/* Called once after enabling the MMU. Panics on the first initialization error. */
void module_init(void);

#endif /* _NUVIX_MODULE_H */

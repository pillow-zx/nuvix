#include <nuvix/module.h>
#include <nuvix/assert.h>
#include <nuvix/printk.h>

extern const struct module_initcall __module_init_start[], __module_init_end[];

void module_init(void)
{
    for (const struct module_initcall *entry = __module_init_start;
         entry < __module_init_end; ++entry) {
        int ret = entry->init();

        if (ret)
            panic("%s: initialization failed (%d)\n", entry->name, ret);
        pr_info("%s: initialization success\n", entry->name);
    }
}

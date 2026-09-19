#ifndef _NUVIX_INIT_H
#define _NUVIX_INIT_H

#include <nuvix/types.h>

struct task_struct;

void kernel_main(uint64_t boot_hartid, paddr_t dtb_pa);
void init_process(void *arg);
bool init_process_is_task(const struct task_struct *task);

#endif

#ifndef _NUVIX_CONFIG_H
#define _NUVIX_CONFIG_H

#include <arch/config.h>

#ifdef CONFIG_SMP
#define NR_CPUS CONFIG_MAX_CPUS
#else
#define NR_CPUS 1
#endif

#endif

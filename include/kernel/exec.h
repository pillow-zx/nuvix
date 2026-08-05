#ifndef _CUTEOS_KERNEL_EXEC_H
#define _CUTEOS_KERNEL_EXEC_H

#include <kernel/compiler.h>
#include <kernel/trap.h>

#define EXEC_MAX_ARGS		16
#define EXEC_MAX_ARG_LEN	128
#define EXEC_MAX_ENVS		16
#define EXEC_MAX_ENV_LEN	128

struct exec_args_envp {
	int argc;
	char argv[EXEC_MAX_ARGS][EXEC_MAX_ARG_LEN];
	int envc;
	char envp[EXEC_MAX_ENVS][EXEC_MAX_ENV_LEN];
};

int kernel_execve(const char *path, const struct exec_args_envp *args, struct trap_frame *tf);

__noreturn
void exec_user_path(const char *path);

#endif

#ifndef _NUVIX_EXECVE_H
#define _NUVIX_EXECVE_H

int init_execve(const char *path, const char *const argv[],
                const char *const envp[]);

#endif

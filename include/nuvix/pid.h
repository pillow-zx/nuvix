#ifndef _NUVIX_PID_H
#define _NUVIX_PID_H

#include <nuvix/cleanup.h>

/* PID and TID share [1, PID_MAX); zero is never allocated. */
#define PID_MAX 32768

/* Reserves a number, without publishing an object; exhaustion returns -EAGAIN. */
int pid_alloc(void);
/* Caller owns a valid reserved number and releases it exactly once. */
void pid_free(unsigned int pid);

DECLARE_FREE(pid, int, if (_T > 0) pid_free(_T))

#endif /* _NUVIX_PID_H */

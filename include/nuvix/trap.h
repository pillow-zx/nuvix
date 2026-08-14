#ifndef _NUVIX_TRAP_H
#define _NUVIX_TRAP_H

#include <arch/trap.h>
#include <uapi/siginfo.h>

enum trap_exception_disposition {
	TRAP_EXCEPTION_SYSCALL,
	TRAP_EXCEPTION_PAGE_FAULT,
	TRAP_EXCEPTION_USER_SIGNAL,
	TRAP_EXCEPTION_KERNEL_FATAL,
};

struct trap_exception {
	enum trap_exception_disposition disposition;
	siginfo_t info;
};

struct trap_exception trap_classify_exception(const struct trap_frame *tf);

#endif

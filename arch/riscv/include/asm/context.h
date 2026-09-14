#ifndef _NUVIX_ASM_CONTEXT_H
#define _NUVIX_ASM_CONTEXT_H


#include <nuvix/types.h>

struct context {
	size_t ra;
	size_t sp;
	size_t s0;
	size_t s1;
	size_t s2;
	size_t s3;
	size_t s4;
	size_t s5;
	size_t s6;
	size_t s7;
	size_t s8;
	size_t s9;
	size_t s10;
	size_t s11;
};

#endif

#ifndef _NUVIX_UAPI_SELECT_H
#define _NUVIX_UAPI_SELECT_H

#define __FD_SETSIZE 1024
#define __NFDBITS    (8 * (int)sizeof(unsigned long))
#define FD_SETSIZE   __FD_SETSIZE

typedef struct {
	unsigned long fds_bits[__FD_SETSIZE / __NFDBITS];
} fd_set;

struct pselect6_sigmask {
	const unsigned long *ss;
	unsigned long ss_len;
};

_Static_assert(sizeof(fd_set) == 128, "fd_set ABI size mismatch");
_Static_assert(sizeof(struct pselect6_sigmask) == 16,
	       "pselect6 sigmask ABI size mismatch");

#define __FD_ELT(fd)	  ((fd) / __NFDBITS)
#define __FD_MASK(fd)	  (1UL << ((fd) % __NFDBITS))
#define FD_ZERO(set)	  __builtin_memset((set), 0, sizeof(*(set)))
#define FD_SET(fd, set)	  ((set)->fds_bits[__FD_ELT(fd)] |= __FD_MASK(fd))
#define FD_CLR(fd, set)	  ((set)->fds_bits[__FD_ELT(fd)] &= ~__FD_MASK(fd))
#define FD_ISSET(fd, set) (((set)->fds_bits[__FD_ELT(fd)] & __FD_MASK(fd)) != 0)

#endif

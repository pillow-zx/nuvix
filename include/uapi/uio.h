#ifndef _NUVIX_UAPI_UIO_H
#define _NUVIX_UAPI_UIO_H

/**
 * @file uio.h
 * @brief Linux iovec UAPI layout.
 */

/**
 * @struct iovec
 * @brief One userspace scatter/gather buffer for readv/writev.
 *
 * @par Fields
 * - @c iov_base: User pointer to the buffer base.
 * - @c iov_len: Buffer length in bytes.
 */
struct iovec {
	void *iov_base;
	unsigned long iov_len;
};

#endif

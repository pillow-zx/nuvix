#ifndef _NUVIX_RANDOM_H
#define _NUVIX_RANDOM_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>

/**
 * @brief Fill a buffer from nuvix's weak boot-time random source.
 *
 * The source is seeded from mtime and task state and is not suitable for
 * cryptographic use.
 */
__nonnull(1) __access_no_size(write_only, 1)
void weak_random_bytes(void *buf, size_t len);

#endif

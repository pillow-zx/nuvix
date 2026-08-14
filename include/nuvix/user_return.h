#ifndef _NUVIX_USER_RETURN_H
#define _NUVIX_USER_RETURN_H

/**
 * @file user_return.h
 * @brief Generic work run before returning from kernel to userspace.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>

struct trap_frame;

/**
 * @brief Run pending work before resuming a user-mode trap frame.
 * @param tf User trap frame that will be restored by the arch trap return.
 */
__nonnull(1)
void user_return_work(struct trap_frame *tf);

#endif

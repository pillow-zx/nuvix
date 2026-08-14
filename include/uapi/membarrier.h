#ifndef _NUVIX_UAPI_MEMBARRIER_H
#define _NUVIX_UAPI_MEMBARRIER_H

/**
 * @file membarrier.h
 * @brief Linux membarrier command and flag UAPI constants.
 *
 * The constants are ABI values from Linux. nuvix currently provides
 * single-core-compatible behavior for supported commands; the bit values must
 * not be renumbered even when an implementation remains shallow.
 */

#define MEMBARRIER_CMD_QUERY				    0
#define MEMBARRIER_CMD_GLOBAL				    (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED			    (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED	    (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED		    (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED	    (1 << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE	    (1 << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1 << 6)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ		    (1 << 7)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ	    (1 << 8)
#define MEMBARRIER_CMD_GET_REGISTRATIONS		    (1 << 9)

/**
 * @def MEMBARRIER_CMD_SHARED
 * @brief Historical Linux alias for the global membarrier command.
 */
#define MEMBARRIER_CMD_SHARED MEMBARRIER_CMD_GLOBAL

/**
 * @def MEMBARRIER_CMD_FLAG_CPU
 * @brief Optional flag meaning the caller supplied a target CPU argument.
 */
#define MEMBARRIER_CMD_FLAG_CPU (1 << 0)

#endif

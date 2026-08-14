#ifndef _NUVIX_TRAP_TYPES_H
#define _NUVIX_TRAP_TYPES_H

/**
 * @file trap_types.h
 * @brief Generic trap classification types shared across arch and MM code.
 */

/**
 * @enum trap_access_type
 * @brief Access class inferred from a trap cause for page-fault handling.
 *
 * @par Fields
 * - @c TRAP_ACCESS_READ: Load or read-like fault.
 * - @c TRAP_ACCESS_WRITE: Store fault.
 * - @c TRAP_ACCESS_EXEC: Instruction-fetch fault.
 */
enum trap_access_type {
	TRAP_ACCESS_READ = 0,
	TRAP_ACCESS_WRITE,
	TRAP_ACCESS_EXEC,
};

#endif

#ifndef _NUVIX_USER_MAP_H
#define _NUVIX_USER_MAP_H

/*
 * include/nuvix/user_map.h - 用户页表特殊映射注册点
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/pgtable.h>

typedef int (*user_map_fn_t)(pte_t *pgd);

__must_check
int user_map_register(const char *name, user_map_fn_t map);

__must_check
int user_map_register_reserved(const char *name, vaddr_t start, vaddr_t end,
		user_map_fn_t map);

__must_check
int user_map_reserve(const char *name, vaddr_t start, vaddr_t end);

__must_check
int user_map_apply(pte_t *pgd);

__must_check __pure
bool user_map_reserved_contains(vaddr_t addr);

__must_check __pure
bool user_map_reserved_overlaps(vaddr_t start, vaddr_t end);

#endif

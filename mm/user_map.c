/*
 * mm/user_map.c - 用户页表特殊映射注册表
 */

#include <nuvix/user_map.h>
#include <nuvix/errno.h>

#define NR_USER_MAPS 4

struct user_map {
	const char *name;
	user_map_fn_t map;
	vaddr_t reserved_start;
	vaddr_t reserved_end;
};

static struct user_map user_maps[NR_USER_MAPS];

int user_map_register(const char *name, user_map_fn_t map)
{
	return user_map_register_reserved(name, 0, 0, map);
}

int user_map_reserve(const char *name, vaddr_t start, vaddr_t end)
{
	return user_map_register_reserved(name, start, end, NULL);
}

int user_map_register_reserved(const char *name, vaddr_t start, vaddr_t end,
			       user_map_fn_t map)
{
	if (!name)
		return -EINVAL;
	if ((start == 0) != (end == 0))
		return -EINVAL;
	if (start && (start >= end || end > TASK_SIZE))
		return -EINVAL;
	if (!map && !start)
		return -EINVAL;

	for (int i = 0; i < NR_USER_MAPS; i++) {
		if (map && user_maps[i].map == map)
			return 0;
		if (!map && !user_maps[i].map &&
		    user_maps[i].reserved_start == start &&
		    user_maps[i].reserved_end == end)
			return 0;
	}

	for (int i = 0; i < NR_USER_MAPS; i++) {
		if (user_maps[i].map || user_maps[i].reserved_end)
			continue;
		user_maps[i].name = name;
		user_maps[i].map = map;
		user_maps[i].reserved_start = start;
		user_maps[i].reserved_end = end;
		return 0;
	}

	return -ENOMEM;
}

bool user_map_reserved_contains(vaddr_t addr)
{
	for (int i = 0; i < NR_USER_MAPS; i++) {
		if (!user_maps[i].reserved_end)
			continue;
		if (addr >= user_maps[i].reserved_start &&
		    addr < user_maps[i].reserved_end)
			return true;
	}

	return false;
}

bool user_map_reserved_overlaps(vaddr_t start, vaddr_t end)
{
	if (start >= end)
		return false;

	for (int i = 0; i < NR_USER_MAPS; i++) {
		if (!user_maps[i].reserved_end)
			continue;
		if (start < user_maps[i].reserved_end &&
		    end > user_maps[i].reserved_start)
			return true;
	}

	return false;
}

int user_map_apply(pte_t *pgd)
{
	int ret;

	if (!pgd)
		return -EINVAL;

	for (int i = 0; i < NR_USER_MAPS; i++) {
		if (!user_maps[i].map)
			continue;

		ret = user_maps[i].map(pgd);
		if (ret < 0)
			return ret;
	}

	return 0;
}

#ifndef _NUVIX_PAGE_MAPPING_H
#define _NUVIX_PAGE_MAPPING_H

/**
 * @file page_mapping.h
 * @brief Logical page-to-physical-block resolver contract.
 */

#include <nuvix/list.h>
#include <nuvix/types.h>

struct page_mapping;

struct page_mapping_ops {
	int (*resolve)(struct page_mapping *mapping, uint64_t index, bool create,
		       uint64_t *block);
};

struct page_mapping {
	void *host;
	dev_t dev;
	const struct page_mapping_ops *ops;
};

static inline void page_mapping_init(struct page_mapping *mapping, void *host, dev_t dev,
		  const struct page_mapping_ops *ops)
{
	if (!mapping)
		return;
	mapping->host = host;
	mapping->dev = dev;
	mapping->ops = ops;
}

#endif

#include <nuvix/string.h>
#include <nuvix/compiler.h>
#include <nuvix/math.h>

void memswap(void *restrict vlhs, void *restrict vrhs, size_t size)
{
	unsigned char *restrict lhs = vlhs;
	unsigned char *restrict rhs = vrhs;

	if (lhs == rhs)
		return;

	const size_t width = ALIGNMENT_OF2((uintptr_t)lhs, (uintptr_t)rhs);


	if (width == sizeof(uint64_t) && size % width == 0) {
		uint64_t *restrict left = assume_aligned(lhs, sizeof(uint64_t));
		uint64_t *restrict right = assume_aligned(rhs, sizeof(uint64_t));

		for (size_t words = size / width; words; words--) {
			uint64_t word = *left;
			*left++ = *right;
			*right++ = word;
		}
		return;
	}

	if (width >= sizeof(uint32_t) && size % sizeof(uint32_t) == 0) {
		uint32_t *restrict left = assume_aligned(lhs, sizeof(uint32_t));
		uint32_t *restrict right = assume_aligned(rhs, sizeof(uint32_t));

		for (size_t words = size / sizeof(*left); words; words--) {
			uint32_t word = *left;
			*left++ = *right;
			*right++ = word;
		}
		return;
	}

	if (width >= sizeof(uint16_t) && size % sizeof(uint16_t) == 0) {
		uint16_t *restrict left = assume_aligned(lhs, sizeof(uint16_t));
		uint16_t *restrict right = assume_aligned(rhs, sizeof(uint16_t));

		for (size_t words = size / sizeof(*left); words; words--) {
			uint16_t word = *left;
			*left++ = *right;
			*right++ = word;
		}
		return;
	}

	while (size--) {
		unsigned char byte = *lhs;
		*lhs++ = *rhs;
		*rhs++ = byte;
	}
}

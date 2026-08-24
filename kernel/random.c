#include <nuvix/random.h>
#include <nuvix/task.h>
#include <nuvix/string.h>
#include <nuvix/timer.h>

static uint64_t random_state;

static uint64_t random_next_u64(void)
{
	uint64_t value = random_state;

	if (value == 0)
		value = timer_now() ^ ((uintptr_t)current_task() << 17) ^
			0x9e3779b97f4a7c15ULL;

	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	random_state = value;
	return value;
}

void weak_random_bytes(void *buf, size_t len)
{
	uint8_t *bytes = buf;

	while (len != 0) {
		uint64_t value = random_next_u64();
		size_t chunk = len < sizeof(value) ? len : sizeof(value);

		memcpy(bytes, &value, chunk);
		bytes += chunk;
		len -= chunk;
	}
}

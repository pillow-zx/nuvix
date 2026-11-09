#ifndef _NUVIX_RANDOM_H
#define _NUVIX_RANDOM_H

#include <nuvix/chacha20.h>

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

void random_seed(const uint8_t seed[CHACHA20_KEY_SIZE]);
void get_random_bytes(void *buf, size_t len);

#endif

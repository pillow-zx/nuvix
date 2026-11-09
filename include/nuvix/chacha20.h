#ifndef _NUVIX_CHACHA20_H
#define _NUVIX_CHACHA20_H

#include <nuvix/ctypes.h>

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_BLOCK_SIZE 64
#define CHACHA20_WORDS      16

#define CHACHA20_INITIALIZER                                                   \
    {.words = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574}}

struct chacha20_state {
    uint32_t words[CHACHA20_WORDS];
};

void chacha20_init(struct chacha20_state *state,
                   const uint8_t key[CHACHA20_KEY_SIZE]);
void chacha20_block(struct chacha20_state *state,
                    uint8_t out[CHACHA20_BLOCK_SIZE]);

#endif

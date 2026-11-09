#include <nuvix/chacha20.h>
#include <nuvix/string.h>

static uint32_t rotate_left(uint32_t value, unsigned int shift)
{
    return (value << shift) | (value >> (32 - shift));
}

static uint32_t load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void quarter_round(uint32_t *x, unsigned int a, unsigned int b,
                          unsigned int c, unsigned int d)
{
    x[a] += x[b];
    x[d] = rotate_left(x[d] ^ x[a], 16);
    x[c] += x[d];
    x[b] = rotate_left(x[b] ^ x[c], 12);
    x[a] += x[b];
    x[d] = rotate_left(x[d] ^ x[a], 8);
    x[c] += x[d];
    x[b] = rotate_left(x[b] ^ x[c], 7);
}

void chacha20_init(struct chacha20_state *state,
                   const uint8_t key[CHACHA20_KEY_SIZE])
{
    *state = (struct chacha20_state)CHACHA20_INITIALIZER;
    for (unsigned int i = 0; i < CHACHA20_KEY_SIZE / sizeof(uint32_t); ++i)
        state->words[4 + i] = load_le32(key + i * sizeof(uint32_t));
}

void chacha20_block(struct chacha20_state *state,
                    uint8_t out[CHACHA20_BLOCK_SIZE])
{
    uint32_t x[CHACHA20_WORDS];

    memcpy(x, state->words, sizeof(x));
    for (unsigned int round = 0; round < 20; round += 2) {
        for (unsigned int i = 0; i < 4; ++i)
            quarter_round(x, i, i + 4, i + 8, i + 12);
        quarter_round(x, 0, 5, 10, 15);
        quarter_round(x, 1, 6, 11, 12);
        quarter_round(x, 2, 7, 8, 13);
        quarter_round(x, 3, 4, 9, 14);
    }
    for (unsigned int i = 0; i < CHACHA20_WORDS; ++i) {
        uint32_t word = x[i] + state->words[i];

        for (unsigned int j = 0; j < sizeof(word); ++j)
            out[i * sizeof(word) + j] = word >> (j * 8);
    }
    ++state->words[12];
}

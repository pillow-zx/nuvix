#include <nuvix/random.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/spinlock.h>
#include <nuvix/string.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/uaccess.h>
#include <asm/trap.h>

#define RANDOM_READ_SIZE 256

static struct {
    spinlock_t lock;
    struct chacha20_state state;
} random = {
    .lock = SPINLOCK_INITIALIZER,
    .state = CHACHA20_INITIALIZER,
};

void random_seed(const uint8_t seed[CHACHA20_KEY_SIZE])
{
    chacha20_init(&random.state, seed);
}

void getrandom(void *buf, size_t len)
{
    uint8_t block[CHACHA20_BLOCK_SIZE];
    uint8_t *out = buf;

    while (len) {
        size_t count = MIN(len, sizeof(block));

        spin_lock(&random.lock);
        chacha20_block(&random.state, block);
        if (!random.state.words[12])
            ++random.state.words[13];
        spin_unlock(&random.lock);
        memcpy(out, block, count);
        out += count;
        len -= count;
    }
}

long sys_getrandom(struct trap_frame *frame)
{
    uintptr_t addr = syscallarg(frame, 0);
    size_t len = syscallarg(frame, 1);
    unsigned int flags = syscallarg(frame, 2);
    uint8_t buffer[RANDOM_READ_SIZE];
    size_t done = 0;

    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
        return -EINVAL;
    if ((flags & (GRND_RANDOM | GRND_INSECURE)) ==
        (GRND_RANDOM | GRND_INSECURE))
        return -EINVAL;
    len = MIN(len, (size_t)INT32_MAX & ~((size_t)PAGE_SIZE - 1));
    while (done < len) {
        size_t count = MIN(len - done, sizeof(buffer));
        size_t uncopied;

        getrandom(buffer, count);
        uncopied = copy_to_user((void *)addr, buffer, count);
        done += count - uncopied;
        if (uncopied)
            return done ? (long)done : -EFAULT;
        addr += count;
        if (signal_pending(current))
            break;
    }
    return done;
}

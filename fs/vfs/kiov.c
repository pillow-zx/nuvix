#include <nuvix/kiov.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/uaccess.h>

void kiov_advance(struct kiov *iov, size_t count)
{
    iov->count -= count;
    while (iov->segs) {
        size_t len = iov->vec->iov_len - iov->offset;

        if (count < len) {
            iov->offset += count;
            break;
        }
        count -= len;
        ++iov->vec;
        --iov->segs;
        iov->offset = 0;
    }
}

static size_t copy_kiov(void *buffer, size_t count, struct kiov *iov, bool to)
{
    size_t done = 0;

    count = MIN(count, iov->count);
    kiov_advance(iov, 0);
    while (done < count) {
        size_t len = MIN(count - done, iov->vec->iov_len - iov->offset);
        void *base = (char *)iov->vec->iov_base + iov->offset;
        char *data = (char *)buffer + done;
        size_t missing = 0;

        if (iov->user) {
            if (to)
                missing = copy_to_user(base, data, len);
            else
                missing = copy_from_user(data, base, len);
        } else if (to) {
            memcpy(base, data, len);
        } else {
            memcpy(data, base, len);
        }
        kiov_advance(iov, len - missing);
        done += len - missing;
        if (missing)
            break;
    }
    return done;
}

size_t copy_to_kiov(const void *buffer, size_t count, struct kiov *iov)
{
    return copy_kiov((void *)buffer, count, iov, true);
}

size_t copy_from_kiov(void *buffer, size_t count, struct kiov *iov)
{
    return copy_kiov(buffer, count, iov, false);
}
